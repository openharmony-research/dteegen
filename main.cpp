#include "CLI11.hpp"
#include "clang-c/CXString.h"
#include <cassert>
#include <clang-c/Index.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#define SECURE "secure"
#define INSECURE "insecure"
#define ENCLAVE "enclave"
#define HOST "host"

enum class WorldType : uint8_t { SECURE_WORLD, INSECURE_WORLD };

#define ASSERT(x, msg)                                                         \
  if (!(x)) {                                                                  \
    std::cerr << msg << std::endl;                                             \
    exit(-1);                                                                  \
  }

struct Param {
  std::string type;
  std::string name;
  int array_size;
};

template <bool WithParam, bool IsEDL>
std::string get_params_str(const std::vector<Param> &params) {
  std::stringstream ss;
  bool flag = false;
  for (const auto &[type, name, array_size] : params) {
    if (flag) {
      ss << ", ";
    }
    if constexpr (IsEDL) {
      if (array_size >= 0) {
        ss << "[in, out, size=" << array_size << "] ";
      }
    }
    if constexpr (WithParam) {
      ss << type << " " << name;
    } else {
      ss << name;
    }
    flag = true;
  }
  return ss.str();
}

std::string get_params(const std::vector<Param> &params) {
  return get_params_str<true, false>(params);
}

std::string get_param_names(const std::vector<Param> &params) {
  return get_params_str<false, false>(params);
}

std::string get_edl_params(const std::vector<Param> &params) {
  return get_params_str<true, true>(params);
}

struct FunctionInfo {
  std::string name;
  std::string returnType;
  std::vector<Param> parameters;
  std::string body;
};

std::vector<FunctionInfo> func_list_each_file;
std::vector<FunctionInfo> secure_entry_func_list;
std::vector<FunctionInfo> insecure_entry_func_list;
using FuncName = std::string;
std::unordered_set<FuncName> func_calls_in_insecure_world;
std::unordered_set<FuncName> func_calls_in_secure_world;
std::unordered_set<FuncName> func_calls_each_file;

std::string getCursorSpelling(const CXCursor &cursor) {
  CXString cxStr = clang_getCursorSpelling(cursor);
  std::string str = clang_getCString(cxStr);
  clang_disposeString(cxStr);
  return str;
}

std::string getTypeSpelling(const CXType &type) {
  CXString cxStr = clang_getTypeSpelling(type);
  std::string str = clang_getCString(cxStr);
  clang_disposeString(cxStr);
  return str;
}

std::string getCursorType(const CXCursor &cursor) {
  CXType type = clang_getCursorType(cursor);
  return getTypeSpelling(type);
}

std::string getFunctionReturnType(CXCursor cursor) {
  CXType returnType = clang_getResultType(clang_getCursorType(cursor));
  return getTypeSpelling(returnType);
}

std::vector<Param> getFunctionParameters(CXCursor cursor) {
  std::vector<Param> parameters;
  int numArgs = clang_Cursor_getNumArguments(cursor);
  for (int i = 0; i < numArgs; ++i) {
    CXCursor arg = clang_Cursor_getArgument(cursor, i);

    const auto type = clang_getCursorType(arg);
    ASSERT(type.kind != CXType_Pointer,
           "In your secure function, please use constant array (e.g. char "
           "arr[32]) instead of pointer "
           "(e.g. char *arr or char arr[])");

    Param param = {.type = getCursorType(arg),
                   .name = getCursorSpelling(arg),
                   .array_size = -1};
    if (type.kind == CXType_ConstantArray) {
      param.array_size = clang_getArraySize(type);
      CXType pointee_type = clang_getArrayElementType(type);
      param.type = getTypeSpelling(pointee_type) + "*";
    }
    parameters.push_back(std::move(param));
  }
  return parameters;
}

std::string g_filepath;

std::string readSourceCode(const std::string &filePath) {
  std::ifstream file(filePath);
  std::stringstream buffer;

  if (file) {
    // 读取文件内容到 buffer
    buffer << file.rdbuf();
    file.close();
    // 将 buffer 转换为 string
    return buffer.str();
  } else {
    // 文件打开失败的处理
    std::cerr << "Unable to open file: " << filePath << std::endl;
    return "";
  }
}

std::string getFunctionBody(CXCursor cursor) {
  // 确保传入的游标是函数声明
  if (clang_getCursorKind(cursor) != CXCursor_FunctionDecl) {
    return ""; // 或者抛出异常
  }

  CXCursor bodyCursor = clang_getNullCursor(); // 初始化为null游标
  clang_visitChildren(
      cursor,
      [](CXCursor c, CXCursor parent, CXClientData clientData) {
        if (clang_getCursorKind(c) == CXCursor_CompoundStmt) {
          // 找到函数体
          *reinterpret_cast<CXCursor *>(clientData) = c;
          return CXChildVisit_Break; // 停止遍历
        }
        return CXChildVisit_Continue; // 继续遍历
      },
      &bodyCursor);
  // 获取函数体的范围
  if (bodyCursor.kind == CXCursor_InvalidCode) {
    return "";
  }
  CXSourceRange range = clang_getCursorExtent(bodyCursor);
  CXSourceLocation startLoc = clang_getRangeStart(range);
  CXSourceLocation endLoc = clang_getRangeEnd(range);

  // 获取源代码的位置信息
  unsigned int startOffset, endOffset;
  clang_getSpellingLocation(startLoc, nullptr, nullptr, nullptr, &startOffset);
  clang_getSpellingLocation(endLoc, nullptr, nullptr, nullptr, &endOffset);

  // 读取源文件内容（这里假设你有途径获取源文件内容）
  std::string sourceCode = readSourceCode(g_filepath); // 需要实现这个函数

  // 提取函数体
  std::string functionBody =
      sourceCode.substr(startOffset, endOffset - startOffset);

  return functionBody;
}

template <WorldType world_type_visited>
CXChildVisitResult func_def_collect_visitor(CXCursor cursor, CXCursor parent,
                                            CXClientData clientData) {
  if (clang_Location_isFromMainFile(clang_getCursorLocation(cursor)) == 0) {
    return CXChildVisit_Continue;
  }

  auto kind = clang_getCursorKind(cursor);
  if (kind == CXCursor_FunctionDecl) {
    auto func_name = getCursorSpelling(cursor);

    bool is_def_valid;
    if constexpr (world_type_visited == WorldType::SECURE_WORLD) {
      is_def_valid = func_calls_in_insecure_world.count(func_name) != 0;
    } else {
      is_def_valid = func_calls_in_secure_world.count(func_name) != 0;
    }

    if (is_def_valid) {
      FunctionInfo funcInfo;
      funcInfo.name = std::move(func_name);
      funcInfo.returnType = getFunctionReturnType(cursor);
      funcInfo.parameters = getFunctionParameters(cursor);
      funcInfo.body = getFunctionBody(cursor);
      if (!funcInfo.body.empty()) {
        func_list_each_file.push_back(std::move(funcInfo));
      }
    }
  }
  return CXChildVisit_Recurse;
}

CXChildVisitResult func_call_collect_visitor(CXCursor cursor, CXCursor parent,
                                             CXClientData clientData) {
  auto kind = clang_getCursorKind(cursor);
  if (kind == CXCursor_CallExpr) {
    cursor = clang_getCursorReferenced(cursor);
    if (clang_getCursorKind(cursor) == CXCursor_FunctionDecl) {
      func_calls_each_file.insert(getCursorSpelling(cursor));
    }
  }
  return CXChildVisit_Recurse;
}

using VISITOR = CXChildVisitResult (*)(CXCursor cursor, CXCursor parent,
                                       CXClientData client_data);

void parse_file(const char *path, VISITOR visitor, void *client_data) {
  CXIndex index = clang_createIndex(0, 0);
  g_filepath = path;
  CXTranslationUnit unit = clang_parseTranslationUnit(
      index, path, nullptr, 0, nullptr, 0, CXTranslationUnit_None);

  if (unit == nullptr) {
    std::cerr << "Unable to parse translation unit. Quitting.\n";
    return;
  }

  CXCursor cursor = clang_getTranslationUnitCursor(unit);
  clang_visitChildren(cursor, visitor, client_data);

  clang_disposeTranslationUnit(unit);
  clang_disposeIndex(index);
}

#define PATTERN(name)                                                          \
  { std::regex("(\\$\\{" #name "\\})"), &SourceContext::name }

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

std::vector<std::pair<std::regex, decltype(&SourceContext::src)>> replaces{
    PATTERN(src),        PATTERN(src_content), PATTERN(ret),
    PATTERN(params),     PATTERN(func_name),   PATTERN(param_names),
    PATTERN(root_cmake), PATTERN(project),     PATTERN(edl_params),
    PATTERN(src_path)};

std::string parse_template(std::string templ, const SourceContext &ctx) {
  std::string res = std::move(templ);
  for (const auto &[pat, repl] : replaces) {
    res = std::regex_replace(res, pat, ctx.*repl);
  }
  return res;
}

std::string get_filepath(std::ifstream &ifs, const SourceContext &ctx) {
  assert(ifs);
  std::string label, filepath;
  ifs >> label;
  assert(label == "path:");

  ifs >> filepath;
  return parse_template(filepath, ctx);
}

std::string get_content(std::ifstream &ifs, const SourceContext &ctx) {
  assert(ifs);
  std::string line;
  std::stringstream ss;
  bool multi_template = false;
  bool global = false;
  bool insecure = false;
  std::vector<std::string> lines;
  while (std::getline(ifs, line)) {
    if (line.find("**begin**") != std::string::npos) {
      multi_template = true;
      continue;
    } else if (line.find("**gbegin**") != std::string::npos) {
      multi_template = true;
      global = true;
      continue;
    } else if (line.find("**igbegin**") != std::string::npos) {
      multi_template = true;
      global = true;
      insecure = true;
      continue;
    } else if (line.find("**end**") != std::string::npos) {

      SourceContext each_ctx = ctx;

      const auto &list = global ? (insecure ? insecure_entry_func_list
                                            : secure_entry_func_list)
                                : func_list_each_file;

      for (const auto &func : list) {
        each_ctx.func_name = func.name;
        each_ctx.params = get_params(func.parameters);
        each_ctx.param_names = get_param_names(func.parameters);
        each_ctx.edl_params = get_edl_params(func.parameters);
        each_ctx.ret = func.returnType;

        for (const auto &line : lines) {
          ss << parse_template(line, each_ctx) << std::endl;
        }
      }

      lines.clear();

      multi_template = false;
      global = false;
      insecure = false;
      continue;
    }
    if (multi_template) {
      lines.push_back(std::move(line));
    } else {
      ss << parse_template(std::move(line), ctx) << std::endl;
    }
  }
  return ss.str();
}

std::string read_file_content(const std::string &filename) {
  std::ifstream ifs(filename);
  std::stringstream ss;
  ss << ifs.rdbuf();
  return ss.str();
}

void process_template(std::ifstream &ifs, const SourceContext &ctx) {
  const auto filepath = get_filepath(ifs, ctx);
  const auto content = get_content(ifs, ctx);

  const auto path = std::filesystem::path("./generated") / filepath;
  std::cout << "GENERATED:" << path << std::endl;
  std::filesystem::create_directories(path.parent_path());
  std::ofstream ofs(path);
  ofs << content;
}

void generate_secgear(const std::filesystem::path project_root) {

  std::filesystem::path generated_path("generated");
  const auto generated_host = generated_path / HOST;
  const auto generated_enclave = generated_path / ENCLAVE;
  const auto secure_root = project_root / SECURE;
  const auto insecure_root = project_root / INSECURE;

  // remove generated first
  if (std::filesystem::exists(generated_path)) {
    std::filesystem::remove_all(generated_path);
  }

  SourceContext ctx;
  ctx.project = project_root.filename();

  for (const auto &insecure_file :
       std::filesystem::recursive_directory_iterator(insecure_root)) {
    if (insecure_file.is_directory()) {
      continue;
    }
    parse_file(insecure_file.path().c_str(), func_call_collect_visitor,
               nullptr);
    func_calls_in_insecure_world.insert(func_calls_each_file.begin(),
                                        func_calls_each_file.end());
    func_calls_each_file.clear();
  }

  for (const auto &secure_file :
       std::filesystem::recursive_directory_iterator(secure_root)) {
    if (secure_file.is_directory()) {
      continue;
    }
    parse_file(secure_file.path().c_str(), func_call_collect_visitor, nullptr);
    func_calls_in_secure_world.insert(func_calls_each_file.begin(),
                                      func_calls_each_file.end());
    func_calls_each_file.clear();
  }

  std::cout << "func_calls_in_insecure_world:" << std::endl;
  for (const auto &func : func_calls_in_insecure_world) {
    std::cout << func << std::endl;
  }
  std::cout << "func_calls_in_secure_world:" << std::endl;
  for (const auto &func : func_calls_in_secure_world) {
    std::cout << func << std::endl;
  }

  for (const auto &secure_func_file :
       std::filesystem::recursive_directory_iterator(secure_root)) {
    if (secure_func_file.is_directory()) {
      continue;
    }

    const auto secure_func_filepath = secure_func_file.path();
    parse_file(secure_func_filepath.c_str(),
               func_def_collect_visitor<WorldType::SECURE_WORLD>, nullptr);

    // not contain definition of secure func
    if (func_list_each_file.empty()) {
      const auto new_path =
          std::filesystem::path("generated") / ENCLAVE /
          (secure_func_filepath.lexically_relative(project_root));
      std::filesystem::create_directories(new_path.parent_path());
      std::filesystem::copy_file(
          secure_func_filepath, new_path,
          std::filesystem::copy_options::overwrite_existing);
      continue;
    }

    ctx.src_path = secure_func_filepath.lexically_relative(project_root);
    ctx.src = secure_func_filepath.stem().string();
    ctx.src_content = read_file_content(secure_func_filepath);

    for (const auto &e : std::filesystem::recursive_directory_iterator(
             "template/secure_func_template")) {
      if (e.is_directory()) {
        continue;
      }
      std::ifstream ifs(e.path());

      process_template(ifs, ctx);
    }

    secure_entry_func_list.insert(secure_entry_func_list.end(),
                                  func_list_each_file.begin(),
                                  func_list_each_file.end());
    func_list_each_file.clear();
  }

  for (const auto &insecure_func_file :
       std::filesystem::recursive_directory_iterator(insecure_root)) {
    if (insecure_func_file.is_directory()) {
      continue;
    }

    const auto insecure_func_filepath = insecure_func_file.path();
    parse_file(insecure_func_filepath.c_str(),
               func_def_collect_visitor<WorldType::INSECURE_WORLD>, nullptr);

    // not contain definition of secure func
    if (func_list_each_file.empty()) {
      continue;
    }

    ctx.src_path = insecure_func_filepath.lexically_relative(project_root);
    ctx.src = insecure_func_filepath.stem().string();
    ctx.src_content = read_file_content(insecure_func_filepath);

    for (const auto &e : std::filesystem::recursive_directory_iterator(
             "template/insecure_func_template")) {
      if (e.is_directory()) {
        continue;
      }
      std::ifstream ifs(e.path());

      process_template(ifs, ctx);
    }

    insecure_entry_func_list.insert(insecure_entry_func_list.end(),
                                    func_list_each_file.begin(),
                                    func_list_each_file.end());
    func_list_each_file.clear();
  }

  const auto root_cmake_path = project_root / "CMakeLists.txt";
  if (std::filesystem::exists(root_cmake_path)) {
    ctx.root_cmake = read_file_content(root_cmake_path);
  }

  for (const auto &e : std::filesystem::recursive_directory_iterator(
           "template/project_template")) {
    if (e.is_directory()) {
      continue;
    }

    std::ifstream ifs(e.path());

    process_template(ifs, ctx);
  }

  // copy to host
  for (const auto &f : std::filesystem::directory_iterator(project_root)) {
    if (f.is_directory()) {
      continue;
    }
    const auto new_path =
        generated_host / f.path().lexically_relative(project_root);
    std::filesystem::copy_options op;
    op |= std::filesystem::copy_options::skip_existing;
    std::filesystem::copy_file(f.path(), new_path, op);
  }

  for (const auto &f :
       std::filesystem::recursive_directory_iterator(project_root / SECURE)) {
    if (f.is_directory()) {
      continue;
    }
    const auto new_path =
        generated_host / f.path().lexically_relative(project_root);
    std::filesystem::create_directories(new_path.parent_path());
    std::filesystem::copy_options op;
    op |= std::filesystem::copy_options::skip_existing;
    std::filesystem::copy_file(f.path(), new_path, op);
  }

  for (const auto &f :
       std::filesystem::recursive_directory_iterator(project_root / INSECURE)) {
    if (f.is_directory()) {
      continue;
    }
    const auto new_path =
        generated_host / f.path().lexically_relative(project_root);
    std::filesystem::create_directories(new_path.parent_path());
    std::filesystem::copy_options op;
    op |= std::filesystem::copy_options::skip_existing;
    std::filesystem::copy_file(f.path(), new_path, op);
  }

  // copy enclave
  for (const auto &f :
       std::filesystem::recursive_directory_iterator(project_root / INSECURE)) {
    if (f.is_directory()) {
      continue;
    }
    if (!(f.path().extension() == ".h")) {
      continue;
    }
    const auto new_path =
        generated_enclave / f.path().lexically_relative(project_root);
    std::filesystem::create_directories(new_path.parent_path());
    std::filesystem::copy_options op;
    op |= std::filesystem::copy_options::skip_existing;
    std::filesystem::copy_file(f.path(), new_path, op);
  }
}

// 主函数
int main(int argc, char **argv) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " project\n";
    return 1;
  }
  generate_secgear(argv[1]);
  return 0;
}
