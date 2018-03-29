/**
  @file  graph_analysis.cc
  @brief Tool for calculating persistent homology of graphs

  This is a tool shipped by 'Aleph - A Library for Exploring Persistent
  Homology'. It uses a simple degree filtration to convert a graph that
  is specified as a list of edges into a simplicial complex.

  Original author: Bastian Rieck
*/

#include <aleph/geometry/RipsExpander.hh>

#include <aleph/persistentHomology/Calculation.hh>

#include <aleph/topology/Simplex.hh>
#include <aleph/topology/SimplicialComplex.hh>

#include <aleph/topology/filtrations/Data.hh>
#include <aleph/topology/filtrations/Degree.hh>

#include <aleph/topology/io/EdgeLists.hh>

#include <aleph/utilities/Filesystem.hh>
#include <aleph/utilities/String.hh>

#include <getopt.h>

#include <iostream>
#include <stdexcept>
#include <vector>

using DataType          = unsigned;
using VertexType        = unsigned;
using Simplex           = aleph::topology::Simplex<DataType, VertexType>;
using SimplicialComplex = aleph::topology::SimplicialComplex<Simplex>;

int main( int argc, char** argv )
{
  static option commandLineOptions[] =
  {
    { "infinity"            , required_argument, nullptr, 'f' },
    { "loops"               , no_argument      , nullptr, 'l' },
    { "zero"                , no_argument      , nullptr, 'z' },
    { nullptr               , 0                , nullptr,  0  }
  };

  DataType infinity           = DataType(2);
  bool calculateLoops         = false;
  bool zeroWeightsForVertices = false;

  {
    int option = 0;
    while( ( option = getopt_long( argc, argv, "f:lz", commandLineOptions, nullptr ) ) != -1 )
    {
      switch( option )
      {
      case 'f':
        infinity = aleph::utilities::convert<DataType>( optarg );
        break;
      case 'l':
        calculateLoops = true;
        break;
      case 'z':
        zeroWeightsForVertices = true;
        break;
      }
    }
  }

  if( ( argc - optind ) < 1 )
    return -1;

  std::string filename = argv[optind++];

  SimplicialComplex K;

  aleph::topology::io::EdgeListReader reader;
  reader.setReadWeights();
  reader.setTrimLines();

  std::cerr << "* Reading '" << filename << "'...";

  reader( filename, K );

  std::cerr << "finished\n"
            << "* Read simplicial complex with " << K.size() << " simplices\n";


  // Calculate degrees -------------------------------------------------

  std::cerr << "* Calculating degree-based filtration...";

  aleph::geometry::RipsExpander<SimplicialComplex> expander;

  unsigned maxDegree = 0;

  {
    std::vector<unsigned> degrees;
    aleph::topology::filtrations::degrees( K, std::back_inserter( degrees ) );

    if( not degrees.empty() )
      maxDegree = *std::max_element( degrees.begin(), degrees.end() );

    K = expander.assignMaximumData( K, degrees.begin(), degrees.end() );
  }

  std::cerr << "finished\n";

  // Set vertex weights ------------------------------------------------

  if( zeroWeightsForVertices )
  {
    std::cerr << "* Setting vertex weights to zero...";

    for( auto it = K.begin(); it != K.end(); ++it )
    {
      if( it->dimension() == 0 )
      {
        auto s = *it;
        s.setData( DataType() );

        auto success = K.replace( it, s );
        if( !success )
          throw std::runtime_error( "Unable to replace simplex" );
      }
    }

    std::cerr << "finished\n";
  }

  // Calculate persistent homology -------------------------------------

  {
    // Establish filtration order of the simplicial complex. The reason
    // we are doing this so late is because client options might change
    // the filtration.
    K.sort( aleph::topology::filtrations::Data<Simplex>() );

    bool dualize                    = true;
    bool includeAllUnpairedCreators = calculateLoops;

    auto diagrams
      = aleph::calculatePersistenceDiagrams( K,
                                             dualize,
                                             includeAllUnpairedCreators );
    for( auto&& diagram : diagrams )
    {
      diagram.removeDiagonal();

      auto output = "/tmp/"
                    + aleph::utilities::basename( filename )
                    + "_d"
                    + std::to_string( diagram.dimension() )
                    + ".txt";

      std::cerr << "* Writing data to \"" << output << "\"...";

      std::ofstream out( output );

      for( auto&& point : diagram )
      {
        if( point.isUnpaired() && infinity > DataType() )
          out << point.x() << "\t" << infinity * maxDegree << "\n";
        else
          out << point.x() << "\t" << point.y() << "\n";
      }

      std::cerr << "finished\n";
    }
  }
}
