#include <stdio.h>
#include <octomap/octomap.h>
#include <octomap/math/Utils.h>
#include <octomap/SemanticOcTree.h>
#include "testing.h"

using namespace std;
using namespace octomap;

#define NUMBER_LABELS 3
#define MAP_RESOLUTION 0.1
#define NUM_EXTRAINFO 4

void printUsage(char* self){
  std::cerr << "\nUSAGE: " << self << " 5 x point_cloud.txt  (point cloud file, required)\n\n";

  exit(1);
}

void print_query_info(point3d query, SemanticOcTreeNode* node) {
  if (node != NULL) {
    cout << "occupancy probability at " << query << ":\t " << node->getOccupancy() << endl;
    cout << "color of node is: " << node->getColor() << endl;
    cout << "Semantics of node is: " << node->getSemantics() << endl;
  }
  else
    cout << "occupancy probability at " << query << ":\t is unknown" << endl;
}


float calcNewOccupancy(double pl, double pg)
{
  double npl = 1.0-pl;
  double npg = 1.0-pg;

  // normalization
  double pg_new = pl*pg;
  double npg_new = npl*npg;
  pg_new = pg_new/(pg_new+npg_new);

  return octomap::logodds(pg_new);
}


std::vector<float> calNewSemantics(
    SemanticOcTreeNode::Semantics sl, SemanticOcTreeNode::Semantics sg) {
  std::vector<float> sg_new;
  for (int i = 0; i < (int)sl.label.size(); i++) {
    float s_new = sl.label[i] * sg.label[i];
    sg_new.push_back(s_new);
  }
  return sg_new;
}


int main(int argc, char** argv) {
  if (argc < 2){
    printUsage(argv[0]);
  }


  // the global tree
  SemanticOcTree tree (MAP_RESOLUTION);


  // +++++++++++++++ START OF BUILDING LOCAL TREE +++++++++++++++++++++++ // 
  SemanticOcTree* localTree = new SemanticOcTree(MAP_RESOLUTION);
  float label[5][5] = {{1, 0, 0, 0, 0}, {0, 1, 0, 0, 0}, {0, 0, 1, 0, 0}, {0, 0, 0, 1, 0}, {0, 0, 0, 0, 1}};
  int color[5][3] = {{255, 255, 255}, {255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 0}};
  
  // prepare label for each class
  std::vector<std::vector<float> > labels;
  labels.resize(NUMBER_LABELS);
  for (int i = 0; i < NUMBER_LABELS; i++) {
    for (int j = 0; j < NUMBER_LABELS; j++)
      labels[i].push_back(label[i][j]);
    //cout<< labels[i] << endl;
  }
  
  // read frame pose from text file
  std::string filename = std::string(argv[1]);
  std::ifstream infile(filename.c_str());

  // read point cloud with extrainfo
  Pointcloud* cloud = new Pointcloud();
  while (infile) {
    cloud->readExtraInfo(infile, NUM_EXTRAINFO);
  }
  cout << "Finish reading the point cloud." << endl;

  // set point cloud origin pose for allignment
  pose6d origin(0, 0, 0, 0, 0, 0);
  
  // insert into OcTree 
  {
    // insert in global coordinates
    localTree->insertPointCloud(*cloud, origin.trans());
    cout << "Finish occupancy mapping." << endl;

    // add semantics to the point cloud
    for (int i = 0; i < (int)cloud->size(); i++) {
      const point3d& query = (*cloud)[i];
      std::vector<float> extra_info = cloud->getExtraInfo(i);
      SemanticOcTreeNode* n = localTree->search (query);
      localTree->averageNodeSemantics(n, labels[ int(extra_info[NUM_EXTRAINFO-1]) ]);
      //print_query_info(query, n);  
    }
  }

  // add semantics to all other nodes
  for (SemanticOcTree::iterator it = localTree->begin(); it != localTree->end(); ++it) {
    if ( !it->isSemanticsSet() ) {
      localTree->averageNodeSemantics((&(*it)), labels[0]);
    }
  }

  cout << "Finish building local TREE" << endl;
  // +++++++++++++++ END OF BUILDING LOCAL TREE +++++++++++++++++++++++ // 


  
  cout << "Expanded num. leafs: " << tree.getNumLeafNodes() << endl;
  // update the global tree according to local tree
  for (SemanticOcTree::leaf_iterator it = localTree->begin_leafs(),
      end = localTree->end_leafs(); it != end; ++it)
  {
    point3d queryCoord = it.getCoordinate();
    SemanticOcTreeNode* n = tree.search(queryCoord);
    if (n==NULL){
      // create a new node in global tree with the same coord
      // set the occupancy log odds
      double pl = it->getOccupancy();
      float ol = octomap::logodds(pl);
      SemanticOcTreeNode* newNode = tree.updateNode(queryCoord, ol);
      // set the semantics
      SemanticOcTreeNode::Semantics sl = it->getSemantics();
      newNode->setSemantics(sl);
      newNode->resetSemanticsCount();
    }
  } 

  
  // +++++++++++++++ START TO UPDATE GLOBAL TREE +++++++++++++++++++++++ // 
  cout << "Expanded num. leafs: " << tree.getNumLeafNodes() << endl;
  // update the global tree according to local tree
  for (SemanticOcTree::leaf_iterator it = localTree->begin_leafs(),
      end = localTree->end_leafs(); it != end; ++it)
  {
    point3d queryCoord = it.getCoordinate();
    SemanticOcTreeNode* n = tree.search(queryCoord);
    double pl = it->getOccupancy();
    SemanticOcTreeNode::Semantics sl = it->getSemantics();
    
    if (n==NULL){
      // create a new node in global tree with the same coord
      // set the occupancy log odds
      float ol = octomap::logodds(pl);
      SemanticOcTreeNode* newNode = tree.updateNode(queryCoord, ol);
      // set the semantics
      newNode->setSemantics(sl);
      newNode->resetSemanticsCount();

    } else{
      // update occupancy prob according to Eq.(7) in paper
      double pg = n->getOccupancy();
      float og_new = calcNewOccupancy(pl, pg);
      n->setLogOdds(og_new);
      
      // if the global node does not have semantics, set as the local value
      if(!n->isSemanticsSet()) {
        OCTOMAP_ERROR("something wrong..");
        continue;
      }

      // else update semantics according to Eq.(7) in paper
      SemanticOcTreeNode::Semantics sg = n->getSemantics();
      std::vector<float> sg_new = calNewSemantics(sl, sg);
      // set new value
      n->setSemantics(sg_new);
      n->normalizeSemantics();
    }
  }
  cout << "Expanded num. leafs: " << tree.getNumLeafNodes() << endl;
  // +++++++++++++++ END OF UPDATING GLOBAL TREE +++++++++++++++++++++++ // 

  // Debug: test if the update is correct
  for (SemanticOcTree::leaf_iterator it = localTree->begin_leafs(),
      end = localTree->end_leafs(); it != end; ++it)
  {
    SemanticOcTreeNode* n = tree.search(it.getCoordinate());
    if (n==NULL){
      OCTOMAP_ERROR("something wrong...\n");
    } else{
      //cout << it->getOccupancy() << " " << n->getOccupancy() << endl;
      //if(n->isSemanticsSet())
        //cout << it->getSemantics().label << it->getSemantics().count << " "
          //<< n->getSemantics().label << n->getSemantics().count << endl;
    }
  }
 
  // traverse the local tree, set color based on semantics to visualize
  for (SemanticOcTree::iterator it = localTree->begin(); it != localTree->end(); ++it) {
    if ( it->isSemanticsSet() ) {
      SemanticOcTreeNode::Semantics s = it->getSemantics();
      // Debug
      //print_query_info(point3d(0,0,0), &(*it)); 
      for (int i = 0; i < NUMBER_LABELS; i++) {
        if (s.label.size() && s.label[i] > 0.6) {
          it->setColor(color[i][0], color[i][1], color[i][2]);
        }
      }
    }
  }//end for

  localTree->write("local_tree.ot");
  delete localTree;


  // traverse the whole tree, set color based on semantics to visualize
  for (SemanticOcTree::iterator it = tree.begin(); it != tree.end(); ++it) {
    if ( it->isSemanticsSet() ) {
      SemanticOcTreeNode::Semantics s = it->getSemantics();
      // Debug
      //print_query_info(point3d(0,0,0), &(*it)); 
      for (int i = 0; i < NUMBER_LABELS; i++) {
        if (s.label.size() && s.label[i] > 0.6) {
          it->setColor(color[i][0], color[i][1], color[i][2]);
        }
      }
    }
  }//end for


  tree.write("global_tree.ot");

  cout << "Test done." << endl;
  exit(0);

}