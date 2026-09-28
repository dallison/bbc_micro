//
//  config.h
//  common_utils
//
//  Created by David Allison on 4/2/21.
//  Copyright © 2021 David Allison. All rights reserved.
//

// This is a general purpose configuration parser.  The syntax is
// based on Google's Protobuf text format.


#ifndef config_h
#define config_h

#include "dstring.h"
#include "map.h"
#include "vector.h"
#include <stdio.h>

typedef enum  {
  kConfigNone,
  kConfigString,
  kConfigInt,
  kConfigVector,
  kConfigObject,
} ConfigNodeType;

typedef struct {
  Map nodes;        // Map of node name (String*) vs ConfigNode.
} ConfigObject;

typedef struct ConfigNode {
  ConfigNodeType type;
  union {
    String string_value;
    int64_t int_value;
    Vector vector_value;      // Vector of ConfigNode*
    ConfigObject* object_value;
  } value;
} ConfigNode;

typedef struct {
  ConfigObject* root;
  String filename;
  int lineno;
  FILE* fp;
  Map symbol_table;     // Map of symbol name vs ConfigNode*.
} ConfigParser;

void ConfigNodeInit(ConfigNode* object);
ConfigNode* NewConfigNode(void);
void ConfigNodeDestruct(ConfigNode* object);
void ConfigNodeDelete(ConfigNode* object);

void ConfigObjectInit(ConfigObject* object);
ConfigObject* NewConfigObject(void);
void ConfigObjectDestruct(ConfigObject* object);
void ConfigObjectDelete(ConfigObject* object);

void ConfigParserInit(ConfigParser* parser, const char* filename);
void ConfigParserDestruct(ConfigParser* parser);

bool ConfigParserParse(ConfigParser* parser);

void ConfigParserPrint(ConfigParser* parser, FILE* fp);
void ConfigNodeVectorize(ConfigNode* node);

ConfigNode* ConfigObjectFind(ConfigObject* obj, const char* name);


#endif /* config_h */
