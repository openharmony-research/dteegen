#pragma once

#include "clang-c/CXString.h"
#include <clang-c/Index.h>
#include <string>
#include <unordered_set>
#include <vector>

struct SourceContext {
  std::string project;
  std::string src;
  std::string src_content;
  std::string ret;
  std::string params;
  std::string param_names;
  std::string edl_params;
  std::string func_name;
  std::string root_cmake;
  std::string src_path;
};

struct Param {
  std::string type;
  std::string name;
  int array_size;
};

struct FunctionInfo {
  std::string name;
  std::string returnType;
  std::vector<Param> parameters;
  std::string body;
};

using VISITOR = CXChildVisitResult (*)(CXCursor cursor, CXCursor parent,
                                       CXClientData client_data);

std::string read_file_content(const std::string &filename);

void parse_file(const char *path, VISITOR visitor, void *client_data);

CXChildVisitResult func_call_collect_visitor(CXCursor cursor, CXCursor parent,
                                             CXClientData clientData);

CXChildVisitResult
insecure_world_func_def_collect_visitor(CXCursor cursor, CXCursor parent,
                                        CXClientData clientData);

CXChildVisitResult
secure_world_func_def_collect_visitor(CXCursor cursor, CXCursor parent,
                                      CXClientData clientData);

void process_template(std::ifstream &ifs, const SourceContext &ctx);

inline std::vector<FunctionInfo> func_list_each_file;
inline std::vector<FunctionInfo> secure_entry_func_list;
inline std::vector<FunctionInfo> insecure_entry_func_list;
using FuncName = std::string;
inline std::unordered_set<FuncName> func_calls_in_insecure_world;
inline std::unordered_set<FuncName> func_calls_in_secure_world;
inline std::unordered_set<FuncName> func_calls_each_file;