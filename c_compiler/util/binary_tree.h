//
//  binary_tree.h
//  c_compiler
//
//  Created by David Allison on 12/31/19.
//  Copyright © 2019 David Allison. All rights reserved.
//

#ifndef binary_tree_h
#define binary_tree_h

#include <stdbool.h>
#include <stddef.h>

//
// This is a balanced binary tree using the red-black tree
// algorithm developed by Xerox PARC in 1972.  Information about
// how this works is available on the internet but a couple of good
// sources are:
//
// Book: Introduction to Algorithms
//       Thomas H Cormen, Charles E Leiserson, Ronald L Rivest
// http://staff.ustc.edu.cn/~csli/graduate/algorithms/book6/chap14.htm
//
// Restatement of the book's pseudo-code in C:
// https://www.cs.auckland.ac.nz/software/AlgAnim/red_black.html
//
// University course:
// http://pages.cs.wisc.edu/~skrentny/cs367-common/readings/Red-Black-Trees/

typedef struct BinaryTreeNode {
  enum Color {
    kBinaryTreeNodeRed,
    kBinaryTreeNodeBlack
  } color;
  struct BinaryTreeNode* left;
  struct BinaryTreeNode* right;
  struct BinaryTreeNode* parent;
  int index;
} BinaryTreeNode;

// Comparison functions:
// return the same as strcmp(node1, node2) for strings
// or node1 - node2 for scalars.
typedef int (*BinaryTreeInsertCompareFunc)(BinaryTreeNode* node1,
                                       BinaryTreeNode* node2);
typedef int (*BinaryTreeSearchCompareFunc)(BinaryTreeNode* node, void* value);

// Called for every node.  Do not free node in this function.
typedef void (*BinaryTreeDestructorFunc)(BinaryTreeNode* node, void* data);

// Called for every node.  Depth starts at 0 and incremented for every
// child.
typedef void (*BinaryTreeTraverseFunc)(BinaryTreeNode* node,
                                       int depth, void* data);

typedef struct BinaryTree {
  BinaryTreeNode* root;
  BinaryTreeInsertCompareFunc insert;
  BinaryTreeSearchCompareFunc search;
  BinaryTreeDestructorFunc destructor;
  int node_count;
} BinaryTree;

void BinaryTreeInit(BinaryTree* tree, BinaryTreeInsertCompareFunc insert,
                    BinaryTreeSearchCompareFunc search,
                    BinaryTreeDestructorFunc destructor);

BinaryTree* NewBinaryTree(BinaryTreeInsertCompareFunc insert,
                          BinaryTreeSearchCompareFunc search,
                          BinaryTreeDestructorFunc destructor);

bool BinaryTreeInsert(BinaryTree* tree, BinaryTreeNode* node);
BinaryTreeNode* BinaryTreeSearch(BinaryTree* tree, void* name);
void BinaryTreeDestruct(BinaryTree* tree, void* data);
void BinaryTreeDelete(BinaryTree* tree, void* data);
void BinaryTreeTraverse(BinaryTree* tree,
                        BinaryTreeTraverseFunc func,
                        void* data);

void BinaryTreeNodeInit(BinaryTreeNode* node);

#endif /* binary_tree_h */
