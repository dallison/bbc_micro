//
//  config.c
//  common_utils
//
//  Created by David Allison on 4/2/21.
//  Copyright © 2021 David Allison. All rights reserved.
//

#include "config.h"
#include <strings.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdarg.h>
#include <inttypes.h>

void ConfigNodeInit(ConfigNode* node) {
  node->type = kConfigNone;
}

ConfigNode* NewConfigNode(void) {
  ConfigNode* node = malloc(sizeof(ConfigNode));
  ConfigNodeInit(node);
  return node;
}

void ConfigNodeDestruct(ConfigNode* node) {
  switch (node->type) {
    case kConfigString:
      StringDestruct(&node->value.string_value);
      break;
    case kConfigVector:
      VectorDestructWithContents(&node->value.vector_value, (VectorElementDestructor)ConfigNodeDestruct, /*free_element=*/true);
      break;
    case kConfigObject:
      ConfigObjectDelete(node->value.object_value);
      break;
    default:
      break;
  }
}

void ConfigNodeDelete(ConfigNode* node) {
  ConfigNodeDestruct(node);
  free(node);
}

static ConfigObject* CopyConfigObject(ConfigObject* obj) {
  ConfigObject* o = NewConfigObject();
  MapInitForStringKeys(&o->nodes);
  MapCopy(&o->nodes, &obj->nodes);
  return o;
}

static ConfigNode* CopyConfigNode(ConfigNode* node) {
  ConfigNode* n = NewConfigNode();
  n->type = node->type;
  switch (n->type) {
    case kConfigInt:
      n->value.int_value = node->value.int_value;
      break;
    case kConfigString:
      StringInit(&n->value.string_value, node->value.string_value.value);
      break;
    case kConfigVector:
      VectorInit(&n->value.vector_value);
      VectorCopy(&n->value.vector_value, &node->value.vector_value);
      break;
    case kConfigObject:
      n->value.object_value = CopyConfigObject(node->value.object_value);
      break;
    case kConfigNone:
      break;
  }
  return n;
}

void ConfigObjectInit(ConfigObject* object) {
  MapInitForStringKeys(&object->nodes);
}

ConfigObject* NewConfigObject(void) {
  ConfigObject* obj = malloc(sizeof(ConfigObject));
  ConfigObjectInit(obj);
  return obj;
}

static void DeleteObjectNode(MapKeyValue* kv) {
  StringDelete((String*)kv->key.p);
  ConfigObjectDelete((ConfigObject*)kv->value.p);
}

void ConfigObjectDestruct(ConfigObject* object) {
  MapDestructWithContents(&object->nodes, DeleteObjectNode);
}

void ConfigObjectDelete(ConfigObject* object) {
  ConfigObjectDestruct(object);
  free(object);
}

static int SkipLine(ConfigParser* parser) {
  while (!feof(parser->fp)) {
    int ch = fgetc(parser->fp);
    if (ch == EOF || ch == '\n') {
      parser->lineno++;
      return ch;
    }
  }
  return EOF;
}

static int ReadChar(ConfigParser* parser) {
  while (!feof(parser->fp)) {
    int ch = fgetc(parser->fp);
    if (ch == EOF) {
      return EOF;
    }
    if (ch == '\n') {
      parser->lineno++;
    }
    if (ch == '#') {
      SkipLine(parser);
      continue;
    }
    return ch;
  }
  return EOF;
}

static void Error(ConfigParser* parser, const char* format, ...) {
  va_list ap;
  va_start(ap, format);
  fprintf(stderr, "%s:%d: ", parser->filename.value, parser->lineno);
  vfprintf(stderr, format, ap);
  fprintf(stderr, "\n");
  va_end(ap);
}

static void ReadWord(ConfigParser* parser, char first, String* word) {
  StringAppendChar(word, first);

  while (!feof(parser->fp)) {
    int ch = ReadChar(parser);
    if (ch == EOF) {
      break;
    }
    if (!isalnum(ch) && ch != '_') {
      ungetc(ch, parser->fp);
      break;
    }
    StringAppendChar(word, ch);
  }
}

static int64_t ReadHexOrOctal(ConfigParser* parser) {
  int64_t n = 0;
  int ch = ReadChar(parser);
  if (ch == 'x' || ch == 'X') {
    while (!feof(parser->fp)) {
      int ch = ReadChar(parser);
      if (ch == EOF) {
        break;
      }
      if (!isxdigit(ch)) {
        ungetc(ch, parser->fp);
        break;
      }
      ch = toupper(ch);
      if (ch >= 'A') {
        ch = ch - 'A' + 10 + '0';
      }
      n = (n << 4) | ch - '0';
    }
  } else {
    ungetc(ch, parser->fp);
    while (!feof(parser->fp)) {
      int ch = ReadChar(parser);
      if (ch == EOF) {
        break;
      }
      if (ch < '0' || ch > '7') {
        ungetc(ch, parser->fp);
        break;
      }
       n = (n << 3) | ch - '0';
    }
    
  }

  return n;

}

static int64_t ReadInt(ConfigParser* parser,char first) {
  if (first == '0') {
    return ReadHexOrOctal(parser);
  }
  int64_t n = first - '0';
  while (!feof(parser->fp)) {
    int ch = ReadChar(parser);
    if (ch == EOF) {
      break;
    }
    if (!isdigit(ch)) {
      ungetc(ch, parser->fp);
      break;
    }
    n = n * 10 + ch - '0';
  }
  return n;
}

// Initial quote has been read.
static COMPILER_UNUSED void ReadString(ConfigParser* parser, String* s) {
  StringInit(s, NULL);
  while (!feof(parser->fp)) {
    int ch = ReadChar(parser);
    if (ch == EOF) {
      break;
    }
    if (ch == '\\') {
      ch = ReadChar(parser);
      if (ch == EOF) {
        break;
      }
      switch (ch) {
        case 'n':
          ch = '\n';
          break;
        case 'r':
          ch = '\r';
          break;
        case 'b':
          ch = '\b';
          break;
        case '\\':
          ch = '\\';
          break;
        case '\'':
          ch = '\'';
          break;
        case '"':
          ch = '"';
          break;
        case '?':
          ch = '?';
          break;
        case 'a':
          ch = '\a';
          break;
        case 'f':
          ch = '\f';
          break;
        case 't':
          ch = '\t';
          break;
        case 'v':
          ch = '\v';
          break;
      }
      StringAppendChar(s, ch);
      continue;
    }
    if (ch == '"') {
      break;
    }
    StringAppendChar(s, ch);
  }
}

static int SkipSpaces(ConfigParser* parser) {
  while (!feof(parser->fp)) {
    int ch = ReadChar(parser);
    if (ch == EOF) {
      return EOF;
    }
    if (!isspace(ch)) {
      return ch;
    }
  }
  return EOF;
}

static void ParseVector(ConfigParser* parser,Vector* vec);

static ConfigNode* ParseConfigNode(ConfigParser* parser,char ch) {
  ConfigNode* node = NewConfigNode();
  if (ch == '"') {
    // String value.
    ReadString(parser, &node->value.string_value);
    node->type = kConfigString;
  } else if (isdigit(ch)) {
    node->value.int_value = ReadInt(parser, ch);
    node->type = kConfigInt;
  } else if (ch == '[') {
    ParseVector(parser, &node->value.vector_value);
    node->type = kConfigVector;
  } else if (isalpha(ch)) {
    // Possible symbol.
    String sym_name = {0};
    ReadWord(parser, ch, &sym_name);
    node = MapFindPointerKey(&parser->symbol_table, &sym_name);
    if (node == NULL) {
      Error(parser, "Undefined symbol %s", sym_name.value);
    } else {
      node = CopyConfigNode(node);
    }
  } else {
    Error(parser, "Invalid config field value");
  }
  return node;
}

static void ParseVector(ConfigParser* parser,Vector* vec) {
  VectorInit(vec);
  int ch = '\0';
  while (!feof(parser->fp)) {
    ch = SkipSpaces(parser);
    if (ch == EOF) {
      break;
    }
    ConfigNode* node = ParseConfigNode(parser, ch);
    VectorAppend(vec, node);
    ch = SkipSpaces(parser);
    if (ch != ',') {
      break;
    }
  }
  // This should be ].  If not, unget.
  if (ch == ']') {
    return;
  }
  Error(parser, "Missing ] for vector value");
  ungetc(ch, parser->fp);
}

void ConfigNodeVectorize(ConfigNode* node) {
  if (node->type == kConfigVector) {
    return;
  }
  ConfigNode* new_node = NewConfigNode();
  new_node->type = node->type;
  switch (node->type) {
    case kConfigVector:
      VectorInit(&new_node->value.vector_value);
      VectorCopy(&new_node->value.vector_value, &node->value.vector_value);
      VectorDestructWithContents(&node->value.vector_value, (VectorElementDestructor)ConfigNodeDestruct, /*free_element=*/true);
      break;
    case kConfigString:
      StringInit(&new_node->value.string_value, node->value.string_value.value);
      StringDestruct(&node->value.string_value);
      break;
    case kConfigInt:
      new_node->value.int_value = node->value.int_value;
      break;
    case kConfigObject:
      new_node->value.object_value = node->value.object_value;
      break;
    case kConfigNone:
      break;
  }
  node->type = kConfigVector;
  VectorInit(&node->value.vector_value);
  VectorAppend(&node->value.vector_value, new_node);
}

static ConfigObject* ParseConfig(ConfigParser* parser) {
  ConfigObject* obj = NewConfigObject();
  while (!feof(parser->fp)) {
    int ch = SkipSpaces(parser);
    if (ch == EOF) {
      break;
    }
    if (ch == '}') {
      break;
    }
    if (ch == '.') {
      String command = {0};
      ReadWord(parser, ch, &command);
      if (StringEqual(&command, ".set")) {
        ch = SkipSpaces(parser);
        String* name = NewString(NULL);
        ReadWord(parser, ch, name);
        ch = SkipSpaces(parser);
        ConfigNode* value = ParseConfigNode(parser, ch);
        ConfigNode* old_value = MapFindPointerKey(&parser->symbol_table, name);
        if (old_value != NULL) {
          Error(parser, "Duplicate symbol %s", name->value);
          StringDelete(name);
          ConfigNodeDelete(value);
        } else {
          MapKeyValue kv;
          kv.key.p = name;
          kv.value.p = value;
          MapInsert(&parser->symbol_table, kv);
        }
      } else {
        Error(parser, "Unknown command %s", command.value);
      }
      continue;
    }
    if (isalpha(ch)) {
      String* name = NewString(NULL);
      ConfigNode* node = NULL;
      
      ReadWord(parser, ch, name);      // read name.
      ch = SkipSpaces(parser);
      if (ch == ':') {
        // Field value follows.
        ch = SkipSpaces(parser);
        node = ParseConfigNode(parser, ch);
      } else if (ch == '{') {
        ConfigObject* node_obj = ParseConfig(parser);
        node = NewConfigNode();
        node->type = kConfigObject;
        node->value.object_value = node_obj;
      }
      
      // We have the name and value for the field.
      ConfigNode* old_node = MapFindPointerKey(&obj->nodes, name);
      if (old_node == NULL) {
        MapKeyValue kv;
        kv.key.p = name;
        kv.value.p = node;
        MapInsert(&obj->nodes, kv);
      } else {
        // Key is already present, vectorize it.
        ConfigNodeVectorize(old_node);
        VectorAppend(&old_node->value.vector_value, node);
        StringDelete(name);
      }
    } else {
      Error(parser, "Syntax error in config file, skipping line");
      SkipLine(parser);
    }
  }
  return obj;
}

struct PrintData {
  int indent;
  FILE* fp;
  const char* sep;  // Vector element seperator.
};

static void DoIndent(struct PrintData* data) {
  for (int i = 0; i < data->indent; i++) {
    fputc(' ', data->fp);
  }
}

static void PrintConfigObjectEntry(MapKeyValue* kv, void* data);

static void PrintConfigNode(ConfigNode* node, struct PrintData* data) {
  FILE* fp = data->fp;
  switch (node->type) {
    case kConfigObject:
      fprintf(fp, " {\n");
      data->indent += 2;
      MapTraverse(&node->value.object_value->nodes, PrintConfigObjectEntry, data);
      data->indent -= 2;
      DoIndent(data);
      fprintf(fp, "}%s", data->sep);
      break;
    case kConfigInt:
      fprintf(fp, "%s%" PRId64 "", data->sep, node->value.int_value);
      break;
    case kConfigString:
      fprintf(fp, "%s\"%s\"", data->sep, node->value.string_value.value);
      break;
    case kConfigVector:
      fprintf(fp, "[");
      data->indent += 2;
      data->sep = "";
      for (size_t i = 0; i < node->value.vector_value.length; i++) {
        ConfigNode* element = node->value.vector_value.value.p[i];
        PrintConfigNode(element, data);
        data->sep = ",";
      }
      data->indent -= 2;
      fprintf(fp, "]");
      break;
    case kConfigNone:
      fprintf(fp, " : <none>\n");
      break;
  }
}

static void PrintConfigObjectEntry(MapKeyValue* kv, void* data) {
  struct PrintData* pdata = data;
  String* name = kv->key.p;
  ConfigNode* node = kv->value.p;
  DoIndent(pdata);
  fprintf(pdata->fp, "%s", name->value);
  struct PrintData sub_data = *pdata;
  
  sub_data.sep = "";
  if (node->type != kConfigObject) {
    fprintf(pdata->fp, ": ");
    sub_data.indent = 0;
  }
  PrintConfigNode(node, &sub_data);
  fprintf(pdata->fp, "\n");
}

void ConfigParserPrint(ConfigParser* parser, FILE* fp) {
  if (parser->root == NULL) {
    return;
  }
  struct PrintData data = {0, fp, ""};
  MapTraverse(&parser->root->nodes, PrintConfigObjectEntry, &data);
}

ConfigNode* ConfigObjectFind(ConfigObject* obj, const char* name) {
  String nm;
  StringInit(&nm, name);
  ConfigNode* node = MapFindPointerKey(&obj->nodes, &nm);
  StringDestruct(&nm);
  return node;
}

void ConfigParserInit(ConfigParser* parser, const char* filename) {
  StringInit(&parser->filename, filename);
  parser->lineno = 1;
  parser->fp = NULL;
  parser->root = NULL;
  MapInitForStringKeys(&parser->symbol_table);
}

void ConfigParserDestruct(ConfigParser* parser) {
  StringDestruct(&parser->filename);
  if (parser->fp != NULL) {
    fclose(parser->fp);
  }
  if (parser->root != NULL) {
    ConfigObjectDelete(parser->root);
  }
}

bool ConfigParserParse(ConfigParser* parser) {
  parser->fp = fopen(parser->filename.value, "r");
  if (parser->fp == NULL) {
    fprintf(stderr, "Linker config file '%s' not found\n", parser->filename.value);
    return false;
  }
  parser->root = ParseConfig(parser);
  fclose(parser->fp);
  parser->fp = NULL;
  return true;
}

