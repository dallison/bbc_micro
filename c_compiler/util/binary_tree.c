//
//  binary_tree.c
//  c_compiler
//
//  Created by David Allison on 12/31/19.
//  Copyright © 2019 David Allison. All rights reserved.
//

#include "binary_tree.h"
#include <stdlib.h>
#include <assert.h>

void BinaryTreeNodeInit(BinaryTreeNode* node) {
  node->color = kBinaryTreeNodeRed;
  node->left = NULL;
  node->right = NULL;
  node->parent = NULL;
  node->index = 0;
}

// Left Tree rotation around x.
//           x                  y
//          / \                / \
//         A   y    --->      x   C
//            / \            / \
//           B   C          A   B
//

// Rotate left:
// y is right child of x
// x->right = y->left
// y->left = x
static void RotateLeft(BinaryTree* tree, BinaryTreeNode* x) {
  // y is new root of subtree.
  BinaryTreeNode* y = x->right;
  x->right = y->left;
  if (y->left != NULL) {
    y->left->parent = x;
  }
  y->parent = x->parent;
  if (x->parent == NULL) {
    tree->root = y;
  } else {
    if (x == x->parent->left) {
      x->parent->left = y;
    } else {
      x->parent->right = y;
    }
  }
  y->left = x;
  x->parent = y;
}

// Rotate right around x
//           x                  y
//          / \                / \
//         y   C     --->     A   x
//        / \                    / \
//       A   B                  B   C
// y->left = x->right
// x->right = y
static void RotateRight(BinaryTree* tree, BinaryTreeNode* x) {
  BinaryTreeNode* y = x->left;
  x->left = y->right;
  if (y->right != NULL) {
    y->right->parent = x;
  }
  y->parent = x->parent;
  if (x->parent == NULL) {
    tree->root = y;
  } else {
    if (x == x->parent->left) {
      x->parent->left = y;
    } else {
      x->parent->right = y;
    }
  }
  y->right = x;
  x->parent = y;
}

static bool InsertNode(BinaryTreeNode** root,
                                 BinaryTreeNode* node,
                                 BinaryTreeNode* parent,
                                 BinaryTreeInsertCompareFunc comp) {
  if (*root == NULL) {
    *root = node;
    node->parent = parent;
    return true;
  }
  int v = comp(*root, node);
  if (v == 0) {
    // Duplicate node
    return false;
  }
  if (v > 0) {
    // Insert to the left.
    return InsertNode(&(*root)->left, node, *root, comp);
  }
  return InsertNode(&(*root)->right, node, *root, comp);
}

// For the tree restructure (if aunt(A) is black):
// Let:
// N = node
// P = parent
// G = grandparent
// Put them in order A, B, C
// If N is left of P then order is A=N, B=P, C=G
// If N is right of P then order is A=P, B=N, C=G
// Restructure tree to make B the parent of A and C, then
// make A and C red and B black.
//
// If the aunt is red we recolor the P, G and A tree as:
// P: black
// U: black
// G: red
static bool BinaryTreeNodeInsert(BinaryTree* tree,
                                 BinaryTreeNode* node,
                                 BinaryTreeInsertCompareFunc comp) {
  // Insert into binary tree using regular algorithm.
  bool ok = InsertNode(&tree->root, node, NULL, comp);
  if (!ok) {
    return false;
  }
  // Now fix the red-black properties from leaf to root.
  BinaryTreeNode* x = node;
  // If node is red and its parent is red we need to fix the tree.
  while (x != tree->root && x->parent->color == kBinaryTreeNodeRed) {
    if (x->parent == x->parent->parent->left) {
      // Left side node.
      BinaryTreeNode* aunt = x->parent->parent->right;
      if (aunt != NULL && aunt->color == kBinaryTreeNodeRed) {
        // aunt is red, recolor parent tree (P, G and aunt)
        // P: black
        // U: black
        // G: red
        x->parent->color = kBinaryTreeNodeBlack;
        aunt->color = kBinaryTreeNodeBlack;
        x->parent->parent->color = kBinaryTreeNodeRed;
        // Move to grandparent and continue up tree.
        x = x->parent->parent;
      } else {
        // aunt is black, need to restructure tree.
        if (x == x->parent->right) {
          // N is to right of P: left rotate P.  N is now P.
          x = x->parent;
          RotateLeft(tree, x);
        }
        // Rotate right around G.
        x->parent->color = kBinaryTreeNodeBlack;
        x->parent->parent->color = kBinaryTreeNodeRed;
        RotateRight(tree, x->parent->parent);
      }
    } else {
      // Right side node, symmetric to left side except we swap
      // right and left.
      BinaryTreeNode* aunt = x->parent->parent->left;
      if (aunt != NULL && aunt->color == kBinaryTreeNodeRed) {
        x->parent->color = kBinaryTreeNodeBlack;
        aunt->color = kBinaryTreeNodeBlack;
        x->parent->parent->color = kBinaryTreeNodeRed;
        x = x->parent->parent;
      } else {
        if (x == x->parent->left) {
          x = x->parent;
          RotateRight(tree, x);
        }
        x->parent->color = kBinaryTreeNodeBlack;
        x->parent->parent->color = kBinaryTreeNodeRed;
        RotateLeft(tree, x->parent->parent);
      }
    }
  }
  tree->root->color = kBinaryTreeNodeBlack;
  return true;
}

static BinaryTreeNode* BinaryTreeNodeSearch(BinaryTreeNode* node,
                                            void* key,
                                            BinaryTreeSearchCompareFunc comp) {
  while (node != NULL) {
    int v = comp(node, key);
    if (v == 0) {
      break;
    }
    if (v > 0) {
      node = node->left;
    } else {
      node = node->right;
    }
  }
  return node;
}

static void BinaryTreeNodeDelete(BinaryTreeNode* node,
                                 void* data,
                                 BinaryTreeDestructorFunc destructor) {
  if (node == NULL) {
    return;
  }
  BinaryTreeNodeDelete(node->left, data, destructor);
  BinaryTreeNodeDelete(node->right, data, destructor);
  if (destructor != NULL) {
    destructor(node, data);
  }
  free(node);
}

static void BinaryTreeNodeTraverse(BinaryTreeNode* node,
                                   BinaryTreeTraverseFunc traverse,
                                   int depth,
                                   void* data) {
  if (node == NULL) {
     return;
   }
   BinaryTreeNodeTraverse(node->right, traverse, depth + 1, data);
   traverse(node, depth, data);
   BinaryTreeNodeTraverse(node->left, traverse, depth + 1, data);
}

void BinaryTreeInit(BinaryTree* tree,
                    BinaryTreeInsertCompareFunc insert,
                    BinaryTreeSearchCompareFunc search,
                    BinaryTreeDestructorFunc destructor) {
  assert(insert != NULL);
  assert(search != NULL);
  tree->root = NULL;
  tree->insert = insert;
  tree->search = search;
  tree->destructor = destructor;
  tree->node_count = 0;
}

BinaryTree* NewBinaryTree(BinaryTreeInsertCompareFunc insert,
                          BinaryTreeSearchCompareFunc search,
                          BinaryTreeDestructorFunc destructor) {
  BinaryTree* tree = malloc(sizeof(BinaryTree));
  BinaryTreeInit(tree, insert, search, destructor);
  return tree;
}

bool BinaryTreeInsert(BinaryTree* tree, BinaryTreeNode* node) {
  assert(tree != NULL);
  assert(node != NULL);
  node->index = ++tree->node_count;
  return BinaryTreeNodeInsert(tree, node, tree->insert);
}

BinaryTreeNode* BinaryTreeSearch(BinaryTree* tree, void* key) {
  if (tree == NULL) {
    return NULL;
  }
  return BinaryTreeNodeSearch(tree->root, key, tree->search);
}

void BinaryTreeDestruct(BinaryTree* tree, void* data) {
  if (tree == NULL) {
    return;
  }
  return BinaryTreeNodeDelete(tree->root, data, tree->destructor);
}

void BinaryTreeDelete(BinaryTree* tree, void* data) {
  if (tree == NULL) {
    return;
  }
  BinaryTreeDestruct(tree, data);
  free(tree);
}

void BinaryTreeTraverse(BinaryTree* tree,
                        BinaryTreeTraverseFunc func,
                        void* data) {
  BinaryTreeNodeTraverse(tree->root, func, 0, data);
}

