#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <string>
#include <sstream>

#include <cassert>
#include <cmath>

// TODO: Replace this as soon as possible with a more modern option
// parser interface.
#include <getopt.h>

#include "filtrations/Data.hh"

#include "geometry/RipsExpander.hh"
#include "geometry/RipsExpanderTopDown.hh"

#include "persistenceDiagrams/Norms.hh"
#include "persistenceDiagrams/PersistenceDiagram.hh"

#include "persistentHomology/ConnectedComponents.hh"

#include "topology/CliqueGraph.hh"
#include "topology/ConnectedComponents.hh"
#include "topology/Simplex.hh"
#include "topology/SimplicialComplex.hh"

#include "topology/io/EdgeLists.hh"
#include "topology/io/GML.hh"
#include "topology/io/Pajek.hh"

#include "utilities/Filesystem.hh"

using DataType           = double;
using VertexType         = unsigned;
using Simplex            = aleph::topology::Simplex<DataType, VertexType>;
using SimplicialComplex  = aleph::topology::SimplicialComplex<Simplex>;
using PersistenceDiagram = aleph::PersistenceDiagram<DataType>;

std::string formatOutput( const std::string& prefix, unsigned k, unsigned K )
{
  std::ostringstream stream;
  stream << prefix;
  stream << std::setw( int( std::log10( K ) + 1 ) ) << std::setfill( '0' ) << k;
  stream << ".txt";

  return stream.str();
}

std::string formatLabel( const std::string label )
{
  // No whitespace---nothing to do
  if( label.find( '\t' ) == std::string::npos && label.find( ' ' ) == std::string::npos )
    return label;
  else
    return "\""+label+"\"";
}

void usage()
{
  std::cerr << "Usage: clique-persistence-diagram [--invert-weights] [--reverse] FILE K\n"
            << "\n"
            << "Calculates the clique persistence diagram for FILE, which is\n"
            << "supposed to be a weighted graph. The K parameter denotes the\n"
            << "maximum dimension of a simplex for extracting a clique graph\n"
            << "and tracking persistence of clique communities.\n\n"
            << ""
            << "******************\n"
            << "Optional arguments\n"
            << "******************\n"
            << "\n"
            << " --centrality    : If specified, calculates centralities for\n"
            << "                   all vertices. Note that this uses copious\n"
            << "                   amounts of time because *all* communities\n"
            << "                   need to be extracted and inspected.\n"
            << "\n"
            << " --invert-weights: If specified, inverts input weights. This\n"
            << "                   is useful if the original weights measure\n"
            << "                   the strength of a relationship, and not a\n"
            << "                   dissimilarity.\n"
            << "\n"
            << " --reverse       : Reverses the enumeration order of cliques\n"
            << "                   by looking for higher-dimensional cliques\n"
            << "                   before enumerating lower-dimensional ones\n"
            << "                   instead of the other way around.\n"
            << "\n\n";
}

int main( int argc, char** argv )
{
  static option commandLineOptions[] =
  {
    { "centrality"    , no_argument      , nullptr, 'c' },
    { "ignore-empty"  , no_argument      , nullptr, 'e' },
    { "invert-weights", no_argument      , nullptr, 'i' },
    { "normalize"     , no_argument      , nullptr, 'n' },
    { "reverse"       , no_argument      , nullptr, 'r' },
    { "min-k"         , required_argument, nullptr, 'k' },
    { nullptr         , 0                , nullptr,  0  }
  };

  bool calculateCentrality = false;
  bool ignoreEmpty         = false;
  bool invertWeights       = false;
  bool normalize           = false;
  bool reverse             = false;
  unsigned minK            = 0;

  int option = 0;
  while( ( option = getopt_long( argc, argv, "k:ceinr", commandLineOptions, nullptr ) ) != -1 )
  {
    switch( option )
    {
    case 'k':
      minK = unsigned( std::stoul( optarg ) );
      break;

    case 'c':
      calculateCentrality = true;
      break;

    case 'e':
      ignoreEmpty = true;
      break;

    case 'i':
      invertWeights = true;
      break;

    case 'n':
      normalize = true;
      break;

    case 'r':
      reverse = true;
      break;

    default:
      break;
    }
  }

  if( (argc - optind ) < 2 )
  {
    usage();
    return -1;
  }

  std::string filename = argv[optind++];
  unsigned maxK        = static_cast<unsigned>( std::stoul( argv[optind++] ) );

  SimplicialComplex K;

  // Input -------------------------------------------------------------

  std::cerr << "* Reading '" << filename << "'...";

  // Optional map of node labels. If the graph contains node labels and
  // I am able to read them, this map will be filled.
  std::map<VertexType, std::string> labels;

  if( aleph::utilities::extension( filename ) == ".gml" )
  {
    aleph::topology::io::GMLReader reader;
    reader( filename, K );

    auto labelMap = reader.getNodeAttribute( "label" );

    // Note that this assumes that the labels are convertible to
    // numbers.
    //
    // TODO: Solve this generically?
    for( auto&& pair : labelMap )
      if( !pair.second.empty() )
        labels[ static_cast<VertexType>( std::stoul( pair.first ) ) ] = pair.second;

    if( labels.empty() )
      labels.clear();
  }
  else if( aleph::utilities::extension( filename ) == ".net" )
  {
    aleph::topology::io::PajekReader reader;
    reader( filename, K );

    auto labelMap = reader.getLabelMap();

    // Note that this assumes that the labels are convertible to
    // numbers.
    //
    // TODO: Solve this generically?
    for( auto&& pair : labelMap )
      if( !pair.second.empty() )
        labels[ static_cast<VertexType>( std::stoul( pair.first ) ) ] = pair.second;

    if( labels.empty() )
      labels.clear();

  }
  else
  {
    aleph::io::EdgeListReader reader;
    reader.setReadWeights( true );
    reader.setTrimLines( true );

    reader( filename, K );
  }

  std::cerr << "finished\n";

  DataType maxWeight = std::numeric_limits<DataType>::lowest();
  DataType minWeight = std::numeric_limits<DataType>::max();
  for( auto&& simplex : K )
  {
    maxWeight = std::max( maxWeight, simplex.data() );
    minWeight = std::min( minWeight, simplex.data() );
  }

  if( normalize && maxWeight != minWeight )
  {
    std::cerr << "* Normalizing weights to [0,1]...";

    auto range = maxWeight - minWeight;

    for (auto it = K.begin(); it != K.end(); ++it )
    {
      if( it->dimension() == 0 )
        continue;

      auto s = *it;

      s.setData( ( s.data() - minWeight ) / range );
      K.replace( it, s );
    }

    maxWeight = DataType(1);
    minWeight = DataType(0);

    std::cerr << "finished\n";
  }

  if( invertWeights )
  {
    std::cerr << "* Inverting filtration weights...";

    for( auto it = K.begin(); it != K.end(); ++it )
    {
      if( it->dimension() == 0 )
        continue;

      auto s = *it;
      s.setData( maxWeight - s.data() );

      K.replace( it, s );
    }

    std::cerr << "finished\n";
  }

  // Expansion ---------------------------------------------------------

  std::cerr << "* Expanding simplicial complex to k=" << maxK << "...";

  if( reverse )
  {
    aleph::geometry::RipsExpanderTopDown<SimplicialComplex> ripsExpander;
    auto L = ripsExpander( K, maxK, minK );
    K      = ripsExpander.assignMaximumWeight( L, K );
  }
  else
  {
    aleph::geometry::RipsExpander<SimplicialComplex> ripsExpander;
    K = ripsExpander( K, maxK );
    K = ripsExpander.assignMaximumWeight( K );
  }

  std::cerr << "finished\n"
            << "* Expanded simplicial complex has " << K.size() << " simplices\n";

  K.sort( aleph::filtrations::Data<Simplex>() );

  // Stores the accumulated persistence of vertices. Persistence
  // accumulates if a vertex participates in a clique community.
  std::map<VertexType, double> accumulatedPersistenceMap;

  // Stores the number of clique communities a vertex is a part of.
  // I am using this only for debugging the algorithm.
  std::map<VertexType, unsigned> numberOfCliqueCommunities;

  std::vector<double> totalPersistenceValues;
  totalPersistenceValues.reserve( maxK );

  // By traversing the clique graphs in descending order I can be sure
  // that a graph will be available. Otherwise, in case of a minimum k
  // parameter and a reverted expansion, only empty clique graphs will
  // be traversed.
  for( unsigned k = maxK; k >= 1; k-- )
  {
    std::cerr << "* Extracting " << k << "-cliques graph...";

    auto C
        = aleph::topology::getCliqueGraph( K, k );

    C.sort( aleph::filtrations::Data<Simplex>() );

    std::cerr << "finished\n";

    std::cerr << "* " << k << "-cliques graph has " << C.size() << " simplices\n";

    if( !ignoreEmpty && C.empty())
    {
      std::cerr << "* Stopping here because no further cliques for processing exist\n";
      break;
    }

    auto&& tuple = aleph::calculateZeroDimensionalPersistenceDiagram<Simplex, aleph::traits::PersistencePairingCalculation<aleph::PersistencePairing<VertexType> > >( C );
    auto&& pd    = std::get<0>( tuple );
    auto&& pp    = std::get<1>( tuple );
    auto&& cs    = std::get<2>( tuple );

    if( calculateCentrality )
    {
      std::cerr << "* Calculating centrality measure (this may take a very long time!)...";

      auto progress = 0.f;
      auto itPoint  = pd.begin();

      for( auto itPair = pp.begin(); itPair != pp.end(); ++itPair )
      {
        // Skip zero-dimensional persistence pairs. This looks somewhat
        // wrong but is correct---recall that we are operating on _two_
        // iterators at once!
        //
        // The persistence pair iterator is 'dumb' and has no knowledge
        // about the persistence values. Hence, we need the iterator of
        // the persistence diagram as well.
        //
        // Note that the diagram must not be modified for this to work!
        if( itPoint->x() == itPoint->y() )
        {
          ++itPoint;
          continue;
        }

        SimplicialComplex filteredComplex;

        {
          std::vector<Simplex> simplices;
          if( itPair->second < C.size() )
          {
            simplices.reserve( itPair->second );

            std::copy( C.begin() + itPair->first, C.begin() + itPair->second, std::back_inserter( simplices ) );
            filteredComplex = SimplicialComplex( simplices.begin(), simplices.end() );
          }
          else
            filteredComplex = C;
        }

        auto uf          = calculateConnectedComponents( filteredComplex );
        auto desiredRoot = *C.at( itPair->first ).begin();
        auto root        = uf.find( desiredRoot ); // Normally, this should be a self-assignment,
                                                   // but in some cases the order of traversal is
                                                   // slightly different, resulting in unexpected
                                                   // roots.

        std::set<VertexType> cliqueVertices;
        std::vector<VertexType> vertices;
        uf.get( root, std::back_inserter( vertices ) );

        for( auto&& vertex : vertices )
        {
          // Notice that the vertex identifier represents the index
          // within the filtration of the _original_ complex, hence
          // I can just access the corresponding simplex that way.
          auto s = K.at( vertex );

          cliqueVertices.insert( s.begin(), s.end() );
        }

        for( auto&& cliqueVertex : cliqueVertices )
        {
          auto persistence = std::isfinite( itPoint->persistence() ) ? std::pow( itPoint->persistence(), 2 ) : std::pow( 2*maxWeight - itPoint->x(), 2 );

          accumulatedPersistenceMap[cliqueVertex] += persistence;
          numberOfCliqueCommunities[cliqueVertex] += 1;
        }

        progress = float( std::distance( pd.begin(), itPoint ) ) / float( pd.size() ) * 100.f;

        if( itPoint == pd.begin() )
          std::cerr << "  0%";
        else
          std::cerr << "\b\b\b\b \b" << std::fixed << std::setw(3) << std::setprecision(0) << progress << "%";

        ++itPoint;
      }

      std::cerr << "finished\n";
    }

    pd.removeDiagonal();

    if( !C.empty() )
    {
      using namespace aleph::utilities;
      auto outputFilename = formatOutput( "/tmp/" + stem( basename( filename ) ) + "_k", k, maxK );

      std::cerr << "* Storing output in '" << outputFilename << "'...\n";

      std::transform( pd.begin(), pd.end(), pd.begin(),
                      [&maxWeight] ( const PersistenceDiagram::Point& p )
                      {
                        if( !std::isfinite( p.y() ) )
                          return PersistenceDiagram::Point( p.x(), 2 * maxWeight );
                        else
                          return PersistenceDiagram::Point( p );
                      } );

      std::ofstream out( outputFilename );
      out << "# Original filename: " << filename << "\n";
      out << "# k                : " << k        << "\n";

      {
        auto itPoint = pd.begin();
        for( auto itPair = pp.begin(); itPair != pp.end(); ++itPair, ++itPoint )
          out << itPoint->x() << "\t" << itPoint->y() << "\t" << cs.at( *C.at( itPair->first ).begin()  ) << "\n";
      }
    }
  }

  {
    using namespace aleph::utilities;
    auto outputFilename = "/tmp/" + stem( basename( filename ) ) + ".txt";

    std::cerr << "* Storing accumulated persistence values in '" << outputFilename << "'...\n";

    std::ofstream out( outputFilename );

    for( auto&& pair : accumulatedPersistenceMap )
      out << pair.first << "\t" << pair.second << "\t" << numberOfCliqueCommunities.at(pair.first) <<  ( labels.empty() ? "" : "\t" + formatLabel( labels.at( pair.first ) ) ) << "\n";
  }
}
