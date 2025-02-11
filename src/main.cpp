#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <queue>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
using namespace std;

extern "C" {
#include <clang-c/CXCompilationDatabase.h>
#include <clang-c/Index.h>
#include <unistd.h>
}

// see https://pugixml.org/docs/manual.html
#include <pugixml.hpp>

// see https://github.com/jarro2783/cxxopts
#include <cxxopts.hpp>

// see https://github.com/gabime/spdlog
#include <spdlog/spdlog.h>

#include "cfg.hpp"
#include "common.hpp"
#include "deduce.hpp"
#include "lmcp.hpp"
#include "mav.hpp"
#include "methods.hpp"
#include "util.hpp"
#include "units.hpp"

#define UNUSED /* UNUSED */

enum MessageDefinitionType { UNKNOWN, MAVLINK, LMCP };

struct ASTContext {
        // maps each mavlink struct name to its frame field
        const map<string, string> &types_to_frame_field;

        // maps each mavlink struct name to a map relating fields to units
        const map<string, map<string, int>> &type_to_field_to_unit;

        // stores the number of units there are
        const int num_units;

        // communicates context to AST walkers
        ConstraintType constraint;
        set<string> &possible_frames;
        bool in_mav_constraint;
        map<unsigned, set<string>> &scope_to_tainted;
        bool had_mav_constraint;
        bool had_taint;

        // maps variables to their type info
        vector<map<string, TypeInfo>> &var_types;

        // maps function names to their summary
        map<string, FunctionSummary> &fn_summary;

        // stores the current function name
        string current_fn;

        // stores the current function usr
        string current_usr;

        // stores the names of the parameters of the current function
        set<string> &current_fn_params;

        // maps the name of parameters to their number in args list
        map<string, int> &param_to_number;

        // maps the parameter number of the current function to the type source
        // kind
        map<int, TypeSourceKind> &param_to_typesource_kind;

        // counts total # of parameters to this function
        int total_params;

        // maps function names to translation units where they appear
        unordered_map<string, set<unsigned>> &name_to_tu;

        // stores if the current function had a definition
        bool had_fn_definition;

        // stores the translation unit number
        unsigned translation_unit_no;

        // stores the name of the current function's semantic context
        // e.g. if we're in a struct T, then stores "T"
        string semantic_context;

        // Stores the set of writes that we're interested in.
        // A write is interesting if it is a write to a variable with a type
        // known a priori.
        const set<string> &writes_to_variables_with_known_types;

        // tracks the current interesting stores
        map<string, TypeInfo> &store_to_typeinfo;

        // stores functions with intrinsic variable types
        set<string> &functions_with_intrinsic_variables;

        // tracks cursors that we've already seen
        unordered_set<string> &seen_definitions;

        // used to coordinate access to shared data structures
        mutex &lock;

        // stores the thread ID currently working
        int thread_no;

        // Relates variables with known types to their types.
        const map<string, TypeInfo> &prior_var_to_typeinfo;

        // Tracks the return types of functions.
        map<string, TypeInfo> &function_names_to_return_unit;

        // Relates unit IDs to their human-readable names.
        const map<int, string> &id_to_unitname;
};

string trim(const string &str, const string &whitespace = " ") {
        const auto strBegin = str.find_first_not_of(whitespace);
        if (strBegin == std::string::npos)
                return ""; // no content

        const auto strEnd = str.find_last_not_of(whitespace);
        const auto strRange = strEnd - strBegin + 1;

        return str.substr(strBegin, strRange);
}

enum CXChildVisitResult type_check_rhs(CXCursor c, CXCursor UNUSED, CXClientData cd);
static thread_local int ctaw_childno = 0;

/**
 * Returns true if the cursor contains a decl ref expr referring to a local
 * variable.
 */
bool contains_local_decl_ref_expr(CXCursor cursor) {
        bool contains = false;
        clang_visitChildren(
            cursor,
            [](CXCursor c, CXCursor UNUSED, CXClientData cd) {
                    bool *con = static_cast<bool *>(cd);
                    CXCursor ref = clang_getCursorReferenced(c);
                    // CXLinkange_NoLinkage implies an auto-scoped variable,
                    // which implies a local variable.
                    if (clang_getCursorKind(c) == CXCursor_DeclRefExpr &&
                        clang_getCursorLinkage(ref) == CXLinkage_NoLinkage) {
                            *con = true;
                            return CXChildVisit_Break;
                    }
                    return CXChildVisit_Recurse;
            },
            &contains);
        return contains;
}

/**
 * Returns the type information associated with varname
 */
optional<TypeInfo>
get_var_typeinfo(const string &varname,
                 const vector<map<string, TypeInfo>> &var_types) {
        for (auto i = 0ul; i < var_types.size(); i++) {
                const auto &m = var_types[var_types.size() - 1 - i];
                const auto &p = m.find(varname);
                if (p != m.end()) {
                        return p->second;
                }
        }
        return {};
}

string get_full_path(CXString compile_dir, CXString filename) {
        const char *filename_cstr = clang_getCString(filename);
        const char *dir_cstr = clang_getCString(compile_dir);
        if (filename_cstr[0] == '/')
                return string(filename_cstr);
        else
                return string(dir_cstr) + string("/") + string(filename_cstr);
}

enum CXChildVisitResult count_left_tokens(CXCursor c, CXCursor UNUSED,
                                          CXClientData cd) {
        unsigned *count = static_cast<unsigned *>(cd);
        CXTranslationUnit unit = clang_Cursor_getTranslationUnit(c);
        CXSourceRange range = clang_getCursorExtent(c);
        CXToken *tokens;
        clang_tokenize(unit, range, &tokens, count);
        clang_disposeTokens(unit, tokens, *count);
        return CXChildVisit_Break;
}

// Returns the binary operator at cursor.
// How this is accomplished:
//   A binary operator has two children.
//   We count the number of tokens of the left child,
//   so then the next token must be the operator.
string get_binary_operator(CXCursor cursor) {
        unsigned left_tokens;
        clang_visitChildren(cursor, count_left_tokens, &left_tokens);

        CXTranslationUnit unit = clang_Cursor_getTranslationUnit(cursor);
        CXSourceRange range = clang_getCursorExtent(cursor);
        CXToken *tokens;
        unsigned count;
        clang_tokenize(unit, range, &tokens, &count);

        string result;
        if (left_tokens < count) {
                CXString token_spelling =
                    clang_getTokenSpelling(unit, tokens[left_tokens]);
                result = string(clang_getCString(token_spelling));
                clang_disposeString(token_spelling);
        }

        clang_disposeTokens(unit, tokens, count);

        return result;
}

// Returns the underlying typename associated with type.
// e.g. if there are qualifiers like const, those are removed.
string get_object_typename(CXType type) {
        // recurse to get pointer types
        if (type.kind == CXType_Pointer)
                return get_object_typename(clang_getPointeeType(type));

        CXString type_spelling = clang_getTypeSpelling(type);
        string result(clang_getCString(type_spelling));
        clang_disposeString(type_spelling);

        size_t pos;
        while ((pos = result.find("const ")) != string::npos)
                result.erase(pos, 6);
        while ((pos = result.find("&")) != string::npos)
                result.erase(pos, 1);

        // TODO: make this function less wrong
        return trim(result);
}

// helps with implementing get_struct_object
enum CXChildVisitResult get_struct_object_helper(CXCursor cursor,
                                                 CXCursor UNUSED,
                                                 CXClientData client_data) {
        pair<bool, optional<string>> *p =
            static_cast<pair<bool, optional<string>> *>(client_data);
        CXCursorKind kind = clang_getCursorKind(cursor);
        if (kind == CXCursor_MemberRefExpr)
                p->first = true;
        if (p->first && kind == CXCursor_DeclRefExpr) {
                // this must be the object name
                p->second = get_cursor_spelling(cursor);
                return CXChildVisit_Break;
        }
        return CXChildVisit_Recurse;
}

// If this operator= stores to a structure field, returns the name of the object
// containing the field.
optional<string> get_struct_object(CXCursor c) {
        pair<bool, optional<string>> p = {false, {}};
        clang_visitChildren(
            c,
            [](CXCursor cursor, CXCursor UNUSED, CXClientData cd) {
                    // we only need to visit the first child (e.g. the lhs)
                    clang_visitChildren(cursor, get_struct_object_helper, cd);
                    return CXChildVisit_Break;
            },
            &p);
        return p.second;
}

// helps with get_first_decl
enum CXChildVisitResult get_first_decl_helper(CXCursor cursor, CXCursor UNUSED,
                                              CXClientData client_data) {
        optional<string> *op = static_cast<optional<string> *>(client_data);
        if (clang_getCursorKind(cursor) == CXCursor_DeclRefExpr) {
                *op = get_cursor_spelling(cursor);
                return CXChildVisit_Break;
        }
        return CXChildVisit_Recurse;
}

// returns the first decl ref expr used in c
optional<string> get_first_decl(CXCursor c) {
        optional<string> result;
        clang_visitChildren(c, get_first_decl_helper, &result);
        return result;
}

enum CXChildVisitResult check_mavlink(CXCursor cursor, CXCursor parent,
                                      CXClientData client_data) {
        ASTContext *ctx = static_cast<ASTContext *>(client_data);
        ctx->in_mav_constraint = false;
        if (clang_getCursorKind(cursor) == CXCursor_DeclRefExpr &&
            clang_getCursorKind(parent) == CXCursor_MemberRefExpr) {
                CXType type = clang_getCursorType(cursor);
                string the_type = get_object_typename(type);

                CXString parent_member = clang_getCursorSpelling(parent);
                string the_parent_member =
                    string(clang_getCString(parent_member));
                clang_disposeString(parent_member);

                const auto &pair = ctx->types_to_frame_field.find(the_type);
                if (pair != ctx->types_to_frame_field.end() &&
                    pair->second == the_parent_member) {
                        ctx->in_mav_constraint = true;
                        ctx->had_mav_constraint = true;
                }

                return CXChildVisit_Break;
        }
        return CXChildVisit_Recurse;
}

enum CXChildVisitResult pretty_print_memberRefExpr_walker(CXCursor c,
                                                          CXCursor UNUSED,
                                                          CXClientData cd) {
        string *sptr = static_cast<string *>(cd);
        CXCursorKind kind = clang_getCursorKind(c);
        CXString cursor_spelling = clang_getCursorSpelling(c);
        string cursor_str = clang_getCString(cursor_spelling);
        clang_disposeString(cursor_spelling);
        if (kind == CXCursor_DeclRefExpr) {
                *sptr = cursor_str + *sptr;
        } else if (kind == CXCursor_MemberRefExpr) {
                *sptr = "::" + cursor_str + *sptr;
        }
        return CXChildVisit_Recurse;
}

// returns a pretty-printed member ref expr
string pretty_print_memberRefExpr(CXCursor c) {
        string result;
        CXString cursor_spelling = clang_getCursorSpelling(c);
        string cursor_str = clang_getCString(cursor_spelling);
        clang_disposeString(cursor_spelling);
        clang_visitChildren(c, pretty_print_memberRefExpr_walker, &result);
        result += "::" + cursor_str;
        return result;
}

enum CXChildVisitResult pretty_print_store_walker(CXCursor c, CXCursor UNUSED,
                                                  CXClientData cd) {
        string *sptr = static_cast<string *>(cd);
        CXCursorKind kind = clang_getCursorKind(c);
        if (kind == CXCursor_MemberRefExpr) {
                *sptr = pretty_print_memberRefExpr(c);
        } else if (kind == CXCursor_ArraySubscriptExpr) {
                return CXChildVisit_Recurse;
        } else {
                CXString cursor_spelling = clang_getCursorSpelling(c);
                *sptr = clang_getCString(cursor_spelling);
                clang_disposeString(cursor_spelling);
        }
        return CXChildVisit_Break;
}

string pretty_print_store(CXCursor c) {
        string result;
        clang_visitChildren(c, pretty_print_store_walker, &result);
        return result;
}

// if c is part of a variable initialization, returns a cursor representing the
// declaration
CXCursor get_initialization_decl(CXCursor c) {
        CXCursor parent = c;
        while (!clang_Cursor_isNull(parent) &&
               clang_getCursorKind(parent) != CXCursor_VarDecl) {
                parent = clang_getCursorSemanticParent(parent);
        }
        return parent;
}

/**
 * Returns a string representing the scope resolution operations.
 * Pre: cursor is a member ref expression.
 */
string get_scope_resolution_operations(CXCursor cursor) {
        string result = "";
        clang_visitChildren(
            cursor,
            [](CXCursor c, CXCursor UNUSED, CXClientData cd) {
                    string *str = static_cast<string *>(cd);
                    CXCursorKind kind = clang_getCursorKind(c);
                    if (kind == CXCursor_DeclRefExpr) {
                            if (str->empty())
                                    *str = get_cursor_spelling(c);
                            else
                                    *str = get_cursor_spelling(c) + "::" + *str;
                            return CXChildVisit_Break;
                    } else if (kind == CXCursor_MemberRefExpr) {
                            string spelling = get_cursor_spelling(c);
                            if (str->empty())
                                    *str = spelling;
                            else
                                    *str = spelling + "::" + *str;
                    }
                    return CXChildVisit_Recurse;
            },
            &result);
        return result;
}

/**
 * Returns if cursor accesses a global variable.
 */
bool is_global_access(CXCursor cursor) {
        bool result;
        clang_visitChildren(
            cursor,
            [](CXCursor c, CXCursor UNUSED, CXClientData cd) {
                    if (clang_getCursorKind(c) == CXCursor_DeclRefExpr) {
                            bool *b = static_cast<bool *>(cd);
                            CXCursor ref = clang_getCursorReferenced(c);
                            if (clang_getCursorLinkage(ref) !=
                                CXLinkage_NoLinkage)
                                    *b = true;
                            else
                                    *b = false;
                            return CXChildVisit_Break;
                    }
                    return CXChildVisit_Recurse;
            },
            &result);
        return result;
}

/**
 * Gets the smallest part of the current semantic context.
 */
string get_smallest_context(const string &semantic_context) {
        if (semantic_context.length() < 3)
                return semantic_context;

        int second_to_last_index = -1, last_index = -1;
        for (size_t i = 0; i < semantic_context.length() - 1; i++) {
                if (semantic_context[i] == ':' &&
                    semantic_context[i + 1] == ':') {
                        second_to_last_index = last_index;
                        last_index = i;
                }
        }

        if (second_to_last_index != -1)
                return semantic_context.substr(second_to_last_index + 2);
        return semantic_context;
}

/**
 * Returns the Scope::Field formatted string of an object access.
 * Pre: cursor is a member ref expression.
 */
string get_member_access_str(ASTContext *ctx, CXCursor cursor) {
        string access_str;
        string scope_ops = get_scope_resolution_operations(cursor);
        string sctx = get_smallest_context(ctx->semantic_context);

        if (scope_ops.empty())
                access_str =
                    ctx->semantic_context + "::" + get_cursor_spelling(cursor);
        else if (is_global_access(cursor))
                access_str = scope_ops + "::" + get_cursor_spelling(cursor);
        else
                access_str = ctx->semantic_context + "::" + scope_ops +
                             "::" + get_cursor_spelling(cursor);
        return access_str;
}

/**
 * t - a known type
 * name - variable name
 * type_to_field_to_unit - relates types to fields to units
 * tinfo - type info
 */
void add_inner_vars(const string &t, const string &name,
                    const map<string, map<string, int>> &type_to_field_to_unit,
                    const TypeSource &source, map<string, TypeInfo> &tinfo) {
        auto typeinfo = type_to_field_to_unit.find(t);
        if (typeinfo == type_to_field_to_unit.end())
                return;
        for (const auto &pair : typeinfo->second) {
                string varname = name + "::" + pair.first;
                tinfo[varname].units.insert(pair.second);

                for (int i = MAV_FRAME_GLOBAL; i < MAV_FRAME_NONE; i++)
                        tinfo[varname].frames.insert(i);

                tinfo[varname].source.push_back(source);
        }
}

// adds a parameter with unknown type to the typeinfo
void add_unknown_param(const string &name, ASTContext *ctx, TypeSource source) {
        TypeInfo ti;
        for (auto frame = 0; frame < MAV_FRAME_NONE; frame++)
                ti.frames.insert(frame);
        for (auto unit = 0; unit < ctx->num_units; unit++)
                ti.units.insert(unit);
        ti.source.push_back(source);
        if (!ctx->var_types.empty())
                ctx->var_types.back()[name] = ti;
}

enum CXChildVisitResult check_tainted_decl_walker(CXCursor c, CXCursor UNUSED,
                                                  CXClientData cd) {
        ASTContext *ctx = static_cast<ASTContext *>(cd);
        CXCursorKind kind = clang_getCursorKind(c);

        // TODO: refactor this into an expression-level type checker.
        string varname = "";
        optional<TypeInfo> ti;
        if (kind == CXCursor_DeclRefExpr) {
                varname = get_cursor_spelling(c);
        } else if (kind == CXCursor_MemberRefExpr) {
                spdlog::trace("(thread {}) pretty printing", ctx->thread_no);
                varname = pretty_print_memberRefExpr(c);
                spdlog::trace("(thread {}) pretty printed", ctx->thread_no);
        } else if (kind == CXCursor_CallExpr) {
                string fq_method_name = get_fq_method(c);
                ctx->lock.lock();
                // See if we know the return type of the method.
                if (ctx->function_names_to_return_unit.find(fq_method_name) == ctx->function_names_to_return_unit.end()) {
                        ctx->lock.unlock();
                        return CXChildVisit_Recurse;
                }
                ti = ctx->function_names_to_return_unit.at(fq_method_name);
                ctx->lock.unlock();
        } else if (kind == CXCursor_BinaryOperator) {
                string spelling = get_binary_operator(c);
                if (spelling == "*") {
                        int old_ctaw = ctaw_childno;
                        ctaw_childno = 1;
                        pair<optional<TypeInfo>, ASTContext *> lhs_p = {{}, ctx};
                        clang_visitChildren(c, type_check_rhs, &lhs_p);

                        pair<optional<TypeInfo>, ASTContext *> rhs_p = {{}, ctx};
                        ctaw_childno = 0;
                        clang_visitChildren(c, type_check_rhs, &rhs_p);
                        ctaw_childno = old_ctaw;

                        if (lhs_p.first && lhs_p.first.value().dimension &&
                            rhs_p.first && rhs_p.first.value().dimension) {
                                    TypeInfo lhs_ti = lhs_p.first.value();
                                    TypeInfo rhs_ti = rhs_p.first.value();
                                    
                                    set<int> frames;
                                    frames.insert(lhs_ti.frames.begin(), lhs_ti.frames.end());
                                    frames.insert(rhs_ti.frames.begin(), rhs_ti.frames.end());

                                    set<int> units;
                                    units.insert(lhs_ti.units.begin(), lhs_ti.units.end());
                                    units.insert(rhs_ti.units.begin(), rhs_ti.units.end());

                                    ti = {
                                            .frames = frames,
                                            .units = units,
                                            .source = {},
                                            .dimension = lhs_ti.dimension.value() * rhs_ti.dimension.value(),
                                    };
                        }
                }
        } else if (kind == CXCursor_IntegerLiteral) {
                CXEvalResult result = clang_Cursor_Evaluate(c);
                int value = clang_EvalResult_getAsInt(result);
                Dimension d = {
                        .coefficients = {0, 0, 0, 0, 0, 0, 0},
                        .scalar_numerator = value,
                        .scalar_denominator = 1,
                };
                ti = {
                        .frames = {},
                        .units = {},
                        .source = {},
                        .dimension = d,
                };
        } else {
                return CXChildVisit_Recurse;
        }

        if (!varname.empty()) {
                ti = get_var_typeinfo(varname, ctx->var_types);
        }

        if (ti.has_value() && !ctx->var_types.empty()) {
                spdlog::trace("(thread {}) getting initialization info",
                              ctx->thread_no);
                CXCursor lhs = get_initialization_decl(c);
                spdlog::trace("(thread {}) got initialization info",
                              ctx->thread_no);
                string new_varname = get_cursor_spelling(lhs);
                ctx->var_types.back()[new_varname] = ti.value();
                return CXChildVisit_Break;
        }

        return CXChildVisit_Recurse;
}

// Checks if cursor stores a mavlink message field into a variable declaration
void check_tainted_decl(CXCursor cursor, ASTContext *ctx) {
        string cursor_typename =
            get_object_typename(clang_getCursorType(cursor));
        bool is_known_type = ctx->types_to_frame_field.find(cursor_typename) !=
                             ctx->types_to_frame_field.end();
        if (is_known_type && !ctx->var_types.empty()) {
                TypeSource source = {SOURCE_INTRINSIC, 0, ""};
                add_inner_vars(cursor_typename, get_cursor_spelling(cursor),
                               ctx->type_to_field_to_unit, source,
                               ctx->var_types.back());
        } else {
                spdlog::trace("(thread {}) walking", ctx->thread_no);
                clang_visitChildren(cursor, check_tainted_decl_walker, ctx);
                spdlog::trace("(thread {}) walked", ctx->thread_no);
        }
}

enum CXChildVisitResult check_tainted_assgn_walker(CXCursor c, CXCursor UNUSED,
                                                   CXClientData cd) {
        if (!ctaw_childno) {
                ctaw_childno++;
                return CXChildVisit_Continue;
        }

        pair<optional<TypeInfo>, ASTContext *> *p =
            static_cast<pair<optional<TypeInfo>, ASTContext *> *>(cd);
        CXCursorKind kind = clang_getCursorKind(c);
        string varname;
        if (kind == CXCursor_MemberRefExpr) {
                varname = get_member_access_str(p->second, c);
        } else if (kind == CXCursor_DeclRefExpr) {
                varname = get_cursor_spelling(c);
        } else if (kind == CXCursor_CallExpr) {
                string fq_method_name = get_fq_method(c);
                ASTContext *ctx = p->second;
                ctx->lock.lock();
                // See if we know the return type of the method.
                if (ctx->function_names_to_return_unit.find(fq_method_name) == ctx->function_names_to_return_unit.end()) {
                        ctx->lock.unlock();
                        return CXChildVisit_Recurse;
                }
                p->first = ctx->function_names_to_return_unit.at(fq_method_name);
                ctx->lock.unlock();
        }

        if (varname == "")
                return CXChildVisit_Recurse;

        p->first = get_var_typeinfo(varname, p->second->var_types);
        const map<string, TypeInfo> &priors = p->second->prior_var_to_typeinfo;
        // See if the variable has a type that was supplied via the prior type switch.
        if (!p->first && priors.find(varname) != priors.end()) {
                p->first = {priors.find(varname)->second};
        }

        if (p->first)
                return CXChildVisit_Break;
        else
                return CXChildVisit_Recurse;

        return CXChildVisit_Recurse;
}

enum CXChildVisitResult type_check_rhs(CXCursor c, CXCursor UNUSED,
                                       CXClientData cd) {
        if (!ctaw_childno) {
                ctaw_childno++;
                return CXChildVisit_Continue;
        }

        pair<optional<TypeInfo>, ASTContext *> *p =
            static_cast<pair<optional<TypeInfo>, ASTContext *> *>(cd);
        CXCursorKind kind = clang_getCursorKind(c);
        string varname;
        if (kind == CXCursor_DeclRefExpr) {
                varname = get_cursor_spelling(c);
        } else if (kind == CXCursor_CallExpr) {
                string fq_method_name = get_fq_method(c);
                p->second->lock.lock();
                // See if we know the return type of the method.
                if (p->second->function_names_to_return_unit.find(fq_method_name) == p->second->function_names_to_return_unit.end()) {
                        p->second->lock.unlock();
                        return CXChildVisit_Recurse;
                }

                TypeInfo ti = p->second->function_names_to_return_unit.at(fq_method_name);
                p->second->lock.unlock();
                p->first = ti;
                return CXChildVisit_Break;
        } else if (kind == CXCursor_BinaryOperator) {
                string spelling = get_binary_operator(c);
                if (spelling == "*") {
                        pair<optional<TypeInfo>, ASTContext *> lhs_p = {{}, p->second};
                        clang_visitChildren(c, type_check_rhs, &lhs_p);

                        int old_ctaw = ctaw_childno;
                        pair<optional<TypeInfo>, ASTContext *> rhs_p = {{}, p->second};
                        ctaw_childno = 0;
                        clang_visitChildren(c, type_check_rhs, &rhs_p);
                        ctaw_childno = old_ctaw;

                        if (lhs_p.first && lhs_p.first.value().dimension &&
                            rhs_p.first && rhs_p.first.value().dimension) {
                                    TypeInfo lhs_ti = lhs_p.first.value();
                                    TypeInfo rhs_ti = rhs_p.first.value();
                                    
                                    set<int> frames;
                                    frames.insert(lhs_ti.frames.begin(), lhs_ti.frames.end());
                                    frames.insert(rhs_ti.frames.begin(), rhs_ti.frames.end());

                                    set<int> units;
                                    units.insert(lhs_ti.units.begin(), lhs_ti.units.end());
                                    units.insert(rhs_ti.units.begin(), rhs_ti.units.end());

                                    p->first = {
                                            .frames = frames,
                                            .units = units,
                                            .source = {},
                                            .dimension = lhs_ti.dimension.value() * rhs_ti.dimension.value(),
                                    };
                                    return CXChildVisit_Break;
                        }
                }
                return CXChildVisit_Recurse;
        } else if (kind == CXCursor_IntegerLiteral) {
                CXEvalResult result = clang_Cursor_Evaluate(c);
                int value = clang_EvalResult_getAsInt(result);
                Dimension d = {
                        .coefficients = {0, 0, 0, 0, 0, 0, 0},
                        .scalar_numerator = value,
                        .scalar_denominator = 1,
                };
                TypeInfo ti = {
                        .frames = {},
                        .units = {},
                        .source = {},
                        .dimension = d,
                };
                p->first = ti;
                return CXChildVisit_Break;
        } else {
                return CXChildVisit_Recurse;
        }

        bool is_param = p->second->current_fn_params.find(varname) !=
                        p->second->current_fn_params.end();
        if (is_param) {
                TypeInfo ti;
                // any frame.
                for (int i = MAV_FRAME_GLOBAL; i < MAV_FRAME_NONE; i++)
                        ti.frames.insert(i);
                for (int i = 0; i < p->second->num_units; i++)
                        ti.units.insert(i);
                TypeSource source = {
                    SOURCE_PARAM,
                    p->second->param_to_number[varname],
                    "",
                };
                ti.source.push_back(source);
                p->first = ti;
                return CXChildVisit_Break;
        } else if (!varname.empty() && !p->second->var_types.empty()) {
                optional<TypeInfo> ti = get_var_typeinfo(varname, p->second->var_types);
                if (ti) {
                        p->first = ti;
                        return CXChildVisit_Break;
                }
        }

        return CXChildVisit_Recurse;
}

// Merges two type infos
void merge_typeinfo(TypeInfo &dst, const TypeInfo &src) {
        const auto &latest_frames = src.frames;
        const auto &latest_units = src.units;
        const auto &sources = src.source;
        dst.frames.insert(latest_frames.begin(), latest_frames.end());
        dst.units.insert(latest_units.begin(), latest_units.end());
        dst.source.insert(dst.source.end(), sources.begin(), sources.end());
}

// Checks if cursor stores (op =) a mavlink message field into another object
void check_tainted_store(CXCursor cursor, ASTContext *ctx) {
        pair<optional<TypeInfo>, ASTContext *> p({}, ctx);
        // ctaw_childno = 0;
        // clang_visitChildren(cursor, check_tainted_assgn_walker, &p);
        // if (!p.first) {
        //         ctaw_childno = 0;
        //         clang_visitChildren(cursor, type_check_rhs, &p);
        // }
        ctaw_childno = 0;
        clang_visitChildren(cursor, type_check_rhs, &p);
        if (!p.first) {
                ctaw_childno = 0;
                clang_visitChildren(cursor, check_tainted_assgn_walker, &p);
        }

        if (p.first && !ctx->var_types.empty()) {
                string varname = pretty_print_store(cursor);

                pair<optional<string>, ASTContext *> data({}, ctx);
                clang_visitChildren(
                    cursor,
                    [](CXCursor c, CXCursor UNUSED, CXClientData cd) {
                            CXCursorKind kind = clang_getCursorKind(c);
                            if ((kind == CXCursor_MemberRefExpr ||
                                 kind == CXCursor_CXXThisExpr) &&
                                !contains_local_decl_ref_expr(c)) {
                                    pair<optional<string>, ASTContext *> *data =
                                        static_cast<pair<optional<string>,
                                                         ASTContext *> *>(cd);
                                    data->first =
                                        get_member_access_str(data->second, c);
                                    ofstream out("/home/ardupilot/variables",
                                                 ios::app);
                                    out << *data->first << endl;
                            }
                            return CXChildVisit_Break;
                    },
                    &data);

                if (!data.first) {
                        data.first = varname;
                }

                if (data.first && ctx->writes_to_variables_with_known_types.find(data.first.value()) != ctx->writes_to_variables_with_known_types.end()) {
                        const TypeInfo &lhs_type_info = ctx->prior_var_to_typeinfo.at(data.first.value());
                        if (p.first != lhs_type_info) {
                                CXSourceLocation location = clang_getCursorLocation(cursor);
                                CXFile file;
                                unsigned line;
                                clang_getSpellingLocation(location, &file, &line, nullptr, nullptr);
                                CXString filename_cx = clang_getFileName(file);
                                string filename = clang_getCString(filename_cx);
                                clang_disposeString(filename_cx);

                                int rhs_type = 0;
                                for (const int type_id : p.first->units) {
                                        rhs_type = type_id;
                                }
                                string rhs_type_name = ctx->id_to_unitname.at(rhs_type);

                                int lhs_type = 0;
                                for (const int type_id : lhs_type_info.units) {
                                        lhs_type = type_id;
                                }
                                string lhs_type_name = ctx->id_to_unitname.at(lhs_type);

                                cout << "Incorrect store to variable " << data.first.value() 
                                     << " in " << filename << " line " << line 
                                     << ". Got type " << rhs_type_name << ", expected type "
                                     << lhs_type_name << "." << endl;
                        }

                        ctx->lock.lock();
                        ctx->functions_with_intrinsic_variables.insert(ctx->current_fn);
                        ctx->lock.unlock();
                        spdlog::trace("(thread {}) found store in {} for {}",
                                      ctx->thread_no, ctx->current_fn,
                                      data.first.value());
                        merge_typeinfo(
                            ctx->store_to_typeinfo[data.first.value()],
                            p.first.value());
                        ctx->var_types.back()[*data.first] = p.first.value();
                } else if (data.first) {
                        // TODO: validate performance on ArduPilot
                        ctx->var_types.back()[data.first.value()] = p.first.value();
                }
        }
}

// Unifies types that appear in last two scope levels
void unify_scopes(map<string, TypeInfo> &old,
                  const map<string, TypeInfo> &latest) {
        for (const auto &p : latest) {
                const auto &it = old.find(p.first);
                if (it != old.end()) {
                        merge_typeinfo(it->second, p.second);
                }
        }
}

enum CXChildVisitResult type_cursor_walker(CXCursor cursor, CXCursor UNUSED,
                                           CXClientData client_data) {
        pair<TypeInfo, ASTContext *> *p =
            static_cast<pair<TypeInfo, ASTContext *> *>(client_data);
        CXCursorKind kind = clang_getCursorKind(cursor);
        if (kind == CXCursor_DeclRefExpr) {
                // check if this is a variable with a known type
                string varname = get_cursor_spelling(cursor);
                optional<TypeInfo> ti =
                    get_var_typeinfo(varname, p->second->var_types);
                if (ti) {
                        p->first = ti.value();
                } else {
                        // this shouldn't happen?
                        for (int i = 0; i < MAV_FRAME_NONE; i++)
                                p->first.frames.insert(i);
                        for (int i = 0; i < p->second->num_units; i++)
                                p->first.units.insert(i);
                        p->first.source.push_back({SOURCE_UNKNOWN, 0, ""});
                }
                return CXChildVisit_Break;
        } else if (kind == CXCursor_MemberRefExpr) {
                string access = pretty_print_memberRefExpr(cursor);
                optional<TypeInfo> ti =
                    get_var_typeinfo(access, p->second->var_types);
                if (ti) {
                        p->first = ti.value();
                        return CXChildVisit_Break;
                } else {
                        optional<string> stored_object = get_first_decl(cursor);
                        if (stored_object) {
                                optional<TypeInfo> ti =
                                    get_var_typeinfo(stored_object.value(),
                                                     p->second->var_types);
                        } else {
                                for (int i = 0; i < MAV_FRAME_NONE; i++)
                                        p->first.frames.insert(i);
                                for (int i = 0; i < p->second->num_units; i++)
                                        p->first.units.insert(i);
                                p->first.source.push_back(
                                    {SOURCE_UNKNOWN, 0, ""});
                        }
                }
        }
        return CXChildVisit_Recurse;
}

// returns the type associated with the cursor at c
TypeInfo type_cursor(CXCursor c, ASTContext *ctx) {
        // Our typing algorithm is simple at the moment.
        //   * If cursor refers to a variable in the ASTContext,
        //     return the type information associated with the variable.
        //   * If cursor refers to another expression, return the first known
        //   type.
        //     If there is no known type, produce a universal type.
        pair<TypeInfo, ASTContext *> p({}, ctx);
        clang_visitChildren(c, type_cursor_walker, &p);
        return p.first;
}

enum CXChildVisitResult function_ast_walker(CXCursor cursor, CXCursor UNUSED,
                                            CXClientData client_data) {
        enum CXChildVisitResult next_action = CXChildVisit_Recurse;

        ASTContext *ctx = static_cast<ASTContext *>(client_data);
        if (ctx->constraint == IFCONDITION) {
                ctx->constraint = UNCONSTRAINED;
                spdlog::trace("(thread {}) in if", ctx->thread_no);
                // check if we're comparing against a mavlink frame
                if (clang_getCursorKind(cursor) == CXCursor_BinaryOperator &&
                    get_binary_operator(cursor) == "==") {
                        clang_visitChildren(cursor, check_mavlink, client_data);
                }
                spdlog::trace("(thread {}) done if", ctx->thread_no);
                return CXChildVisit_Continue;
        } else if (ctx->constraint == SWITCHSTMT) {
                // this indicates the cursor is the control expression of a
                // switch statement. we'll visit this control expression and see
                // if it's on a mavlink field.
                spdlog::trace("(thread {}) in switch", ctx->thread_no);
                ctx->constraint = UNCONSTRAINED;
                clang_visitChildren(cursor, check_mavlink, client_data);
                spdlog::trace("(thread {}) done switch", ctx->thread_no);
                return CXChildVisit_Break;
        }

        // find all if statements that constrain mavlink messages
        CXCursorKind kind = clang_getCursorKind(cursor);
        if (kind == CXCursor_IfStmt) {
                spdlog::trace("(thread {}) in if", ctx->thread_no);
                ctx->constraint = IFCONDITION;

                // Set up a new scope for the body of the if statement.
                // Subsequent definitions will affect the if statement's scope.
                // The winning definition (e.g. last stores in if)
                // are propagated to the current scope.
                map<string, TypeInfo> if_var_types;
                ctx->var_types.push_back(if_var_types);

                // Use the new scope.
                clang_visitChildren(cursor, function_ast_walker, client_data);

                // Unify scopes.
                unify_scopes(ctx->var_types[ctx->var_types.size() - 2],
                             ctx->var_types.back());

                // Destroy latest scope.
                ctx->var_types.pop_back();

                // We've already looked at the child of the if statement.
                // Time to move on.
                next_action = CXChildVisit_Continue;
                spdlog::trace("(thread {}) done if", ctx->thread_no);
        } else if (kind == CXCursor_ForStmt || kind == CXCursor_WhileStmt) {
                spdlog::trace("(thread {}) in for", ctx->thread_no);
                // Set up a new scope for the body of the loop statement.
                // Subsequent definitions will affect the loop statement's
                // scope. The winning definitions (e.g. last stores in the loop)
                // are propagated to the current scope.
                map<string, TypeInfo> loop_var_types;
                ctx->var_types.push_back(loop_var_types);

                // Use the new scope.
                clang_visitChildren(cursor, function_ast_walker, client_data);

                // Unify scopes.
                unify_scopes(ctx->var_types[ctx->var_types.size() - 2],
                             ctx->var_types.back());

                // Destroy latest scope.
                ctx->var_types.pop_back();

                // We've already looked at the child of the loop statement.
                // Time to move on.
                next_action = CXChildVisit_Continue;
                spdlog::trace("(thread {}) done for", ctx->thread_no);
        } else if (kind == CXCursor_BreakStmt) {
                spdlog::trace("(thread {}) in break", ctx->thread_no);
                // A break causes an instant unification of the type
                // information.
                unify_scopes(ctx->var_types[ctx->var_types.size() - 2],
                             ctx->var_types.back());
                spdlog::trace("(thread {}) done break", ctx->thread_no);
        } else if (kind == CXCursor_SwitchStmt) {
                spdlog::trace("(thread {}) in switch", ctx->thread_no);

                ctx->constraint = SWITCHSTMT;
                clang_visitChildren(cursor, function_ast_walker, client_data);
                if (ctx->in_mav_constraint) {
                        cout << "Found a MAVLink frame switch!" << endl;
                }

                // Set up a new scope for the body of the switch statement.
                // Subsequent definitions will affect the switch statement's
                // scope. The winning definitions (e.g. last stores in the
                // switch) are propagated to the current scope.
                map<string, TypeInfo> switch_var_types;
                ctx->var_types.push_back(switch_var_types);

                // Use the new scope.
                clang_visitChildren(cursor, function_ast_walker, client_data);

                // Unify scopes.
                unify_scopes(ctx->var_types[ctx->var_types.size() - 1],
                             ctx->var_types.back());

                // Destroy latest scope.
                ctx->var_types.pop_back();

                // We've already looked at the child of the switch statement.
                // Time to move on.
                next_action = CXChildVisit_Continue;
                spdlog::trace("(thread {}) done switch", ctx->thread_no);
        } else if (kind == CXCursor_VarDecl) {
                spdlog::trace("(thread {}) in var decl", ctx->thread_no);
                check_tainted_decl(cursor, ctx);
                spdlog::trace("(thread {}) checked tainted decl",
                              ctx->thread_no);
                CXType t = clang_getCursorType(cursor);
                string t_type = get_object_typename(t);
                bool is_mav_type = ctx->types_to_frame_field.find(t_type) !=
                                       ctx->types_to_frame_field.end() ||
                                   ctx->type_to_field_to_unit.find(t_type) !=
                                       ctx->type_to_field_to_unit.end();
                if (is_mav_type) {
                        ctx->lock.lock();
                        ctx->functions_with_intrinsic_variables.insert(
                            ctx->current_fn);
                        ctx->lock.unlock();
                        if (ctx->types_to_frame_field.find(t_type) !=
                            ctx->types_to_frame_field.end())
                                ctx->had_taint = true;
                }

                spdlog::trace("(thread {}) done var decl", ctx->thread_no);
                // TODO: use taint information
        } else if (kind == CXCursor_BinaryOperator) {
                spdlog::trace("(thread {}) in binop", ctx->thread_no);
                string op = get_binary_operator(cursor);
                if (op == "=") {
                        if (ctx->current_fn == "InitialiseVariables") {
                                cout << "checking taint in InitialiseVariables"
                                     << endl;
                        }
                        check_tainted_store(cursor, ctx);
                        // TODO: use taint information
                }
                spdlog::trace("(thread {}) done binop", ctx->thread_no);
        } else if (kind == CXCursor_CallExpr) {
                spdlog::trace("(thread {}) in call expr", ctx->thread_no);
                string spelling = get_cursor_spelling(cursor);
                if (spelling == "operator=") {
                        check_tainted_store(cursor, ctx);
                        // TODO: use taint information
                } else if (!spelling.empty()) {
                        int num_args = clang_Cursor_getNumArguments(cursor);
                        vector<TypeInfo> call_info;
                        for (int i = 0; i < num_args; i++) {
                                CXCursor arg =
                                    clang_Cursor_getArgument(cursor, i);
                                TypeInfo t = type_cursor(arg, ctx);
                                call_info.push_back(t);
                        }

                        string this_fn = ctx->current_fn;
                        ctx->lock.lock();
                        ctx->fn_summary[this_fn].callees.insert(spelling);
                        ctx->fn_summary[this_fn]
                            .calling_context[spelling]
                            .push_back(call_info);
                        ctx->lock.unlock();
                }
                spdlog::trace("(thread {}) done call expr", ctx->thread_no);
        } else if (kind == CXCursor_ParmDecl) {
                spdlog::trace("(thread {}) in parm decl", ctx->thread_no);
                CXType t = clang_getCursorType(cursor);
                string t_type = get_object_typename(t);
                bool is_mav_type = ctx->types_to_frame_field.find(t_type) !=
                                       ctx->types_to_frame_field.end() ||
                                   ctx->type_to_field_to_unit.find(t_type) !=
                                       ctx->type_to_field_to_unit.end();
                string param_name = get_cursor_spelling(cursor);
                ctx->param_to_number[param_name] = ctx->total_params;
                if (is_mav_type) {
                        TypeSource source = {SOURCE_INTRINSIC,
                                             ctx->total_params, ""};
                        add_inner_vars(t_type, param_name,
                                       ctx->type_to_field_to_unit, source,
                                       ctx->var_types.back());
                        ctx->param_to_typesource_kind[ctx->total_params] =
                            SOURCE_INTRINSIC;
                        ctx->lock.lock();
                        ctx->functions_with_intrinsic_variables.insert(
                            ctx->current_fn);
                        ctx->lock.unlock();
                } else {
                        // TODO: should this really be unknown?
                        ctx->param_to_typesource_kind[ctx->total_params] =
                            SOURCE_UNKNOWN;
                        ctx->current_fn_params.insert(param_name);
                        TypeSource source = {SOURCE_PARAM, ctx->total_params,
                                             ""};
                        add_unknown_param(param_name, ctx, source);
                }
                ctx->total_params++;
                spdlog::trace("(thread {}) done parm decl", ctx->thread_no);
        } else if (kind == CXCursor_CompoundStmt) {
                spdlog::trace("(thread {}) in compound statement",
                              ctx->thread_no);
                if (!ctx->had_fn_definition) {
                        ctx->lock.lock();
                        if (ctx->seen_definitions.find(ctx->current_usr) ==
                            ctx->seen_definitions.end())
                                ctx->seen_definitions.insert(ctx->current_usr);
                        else
                                // this is tricky part i
                                ctx->had_fn_definition = true;
                        ctx->lock.unlock();
                        // this is tricky part ii
                        ctx->had_fn_definition = !ctx->had_fn_definition;
                }
                spdlog::trace("(thread {}) done compound statement",
                              ctx->thread_no);
        }

        return next_action;
}

string get_cursor_usr(CXCursor cursor) {
        CXString str = clang_getCursorUSR(cursor);
        string result(clang_getCString(str));
        clang_disposeString(str);
        return result;
}

enum CXChildVisitResult ast_walker(CXCursor cursor, CXCursor UNUSED,
                                   CXClientData client_data) {
        CXCursorKind kind = clang_getCursorKind(cursor);
        if (kind == CXCursor_FunctionDecl || kind == CXCursor_CXXMethod) {
                // TODO: handle overloading + overriding
                ASTContext *ctx = static_cast<ASTContext *>(client_data);
                string usr = get_cursor_usr(cursor);

                // checks if we already visited this function
                ctx->lock.lock();
                if (ctx->seen_definitions.find(usr) !=
                    ctx->seen_definitions.end()) {
                        ctx->lock.unlock();
                        return CXChildVisit_Continue;
                }
                ctx->lock.unlock();

                ctx->had_mav_constraint = false;
                ctx->had_taint = false;
                ctx->current_fn = get_cursor_spelling(cursor);
                ctx->current_usr = usr;

                map<string, TypeInfo> scope;
                ctx->var_types.push_back(scope);
                ctx->had_fn_definition = false;

                spdlog::trace("(thread {}) working in {}", ctx->thread_no,
                              ctx->current_fn);
                if (ctx->current_fn == "InitialiseVariables") {
                        cout << "working in InitialiseVariables" << endl;
                }

                int old_ctx_len = ctx->semantic_context.length();
                if (kind == CXCursor_CXXMethod) {
                        CXCursor semantic_parent =
                            clang_getCursorSemanticParent(cursor);
                        string semantic_spelling =
                            get_cursor_spelling(semantic_parent);
                        if (ctx->semantic_context.empty())
                                ctx->semantic_context = semantic_spelling;
                        else
                                ctx->semantic_context =
                                    ctx->semantic_context +
                                    "::" + semantic_spelling;
                }

                clang_visitChildren(cursor, function_ast_walker, client_data);

                if (ctx->had_taint && ctx->had_fn_definition &&
                    !ctx->had_mav_constraint) {
                        CXString cursor_spelling =
                            clang_getCursorSpelling(cursor);
                        cout << "BUG: unconstrained MAV frame used in: "
                             << clang_getCString(cursor_spelling) << endl;
                        clang_disposeString(cursor_spelling);
                }

                if (ctx->had_fn_definition) {
                        string name = get_cursor_spelling(cursor);
                        ctx->lock.lock();
                        ctx->fn_summary[name].num_params = ctx->total_params;
                        ctx->fn_summary[name].param_to_typesource_kind.swap(
                            ctx->param_to_typesource_kind);
                        ctx->name_to_tu[name].insert(ctx->translation_unit_no);
                        ctx->fn_summary[name].store_to_typeinfo.swap(
                            ctx->store_to_typeinfo);
                        ctx->lock.unlock();
                }

                // clean up this function's mess
                ctx->var_types.pop_back();
                ctx->current_fn_params.clear();
                ctx->param_to_number.clear();
                ctx->param_to_typesource_kind.clear();
                ctx->total_params = 0;
                if (kind == CXCursor_CXXMethod)
                        ctx->semantic_context.erase(old_ctx_len);
                ctx->store_to_typeinfo.clear();

                spdlog::trace("(thread {}) done with {}", ctx->thread_no,
                              ctx->current_fn);
                return CXChildVisit_Continue;
        }
        // TODO: handle global variable declarations

        return CXChildVisit_Recurse;
}

static map<string, TypeInfo>
vars_to_typeinfo(const vector<VariableEntry> &vars,
                 map<string, int> &unit_to_id,
                 int &type_id) {
        map<string, TypeInfo> results;
        const map<string, MAVFrame> frame_to_field = {
            {"MAV_FRAME_GLOBAL", MAV_FRAME_GLOBAL},
            {"MAV_FRAME_LOCAL_NED", MAV_FRAME_LOCAL_NED},
            {"MAV_FRAME_MISSION", MAV_FRAME_MISSION},
            {"MAV_FRAME_GLOBAL_RELATIVE_ALT", MAV_FRAME_GLOBAL_RELATIVE_ALT},
            {"MAV_FRAME_LOCAL_ENU", MAV_FRAME_LOCAL_ENU},
            {"MAV_FRAME_GLOBAL_INT", MAV_FRAME_GLOBAL_INT},
            {"MAV_FRAME_GLOBAL_RELATIVE_ALT_INT",
             MAV_FRAME_GLOBAL_RELATIVE_ALT_INT},
            {"MAV_FRAME_LOCAL_OFFSET_NED", MAV_FRAME_LOCAL_OFFSET_NED},
            {"MAV_FRAME_BODY_NED", MAV_FRAME_BODY_NED},
            {"MAV_FRAME_BODY_OFFSET_NED", MAV_FRAME_BODY_OFFSET_NED},
            {"MAV_FRAME_GLOBAL_TERRAIN_ALT", MAV_FRAME_GLOBAL_TERRAIN_ALT},
            {"MAV_FRAME_GLOBAL_TERRAIN_ALT_INT",
             MAV_FRAME_GLOBAL_TERRAIN_ALT_INT},
            {"MAV_FRAME_BODY_FRD", MAV_FRAME_BODY_FRD},
            {"MAV_FRAME_LOCAL_FRD", MAV_FRAME_LOCAL_FRD},
            {"MAV_FRAME_LOCAL_FLU", MAV_FRAME_LOCAL_FLU},
            {"MAV_FRAME_NONE", MAV_FRAME_NONE}};
        for (const auto &ve : vars) {
                TypeInfo ti;
                for (const auto &fr : ve.semantic_info.coordinate_frames) {
                        const auto &it = frame_to_field.find(fr);
                        if (it == frame_to_field.end())
                                ti.frames.insert(MAV_FRAME_NONE);
                        else
                                ti.frames.insert(it->second);
                }
                for (const auto &unit_name : ve.semantic_info.units) {
                        const auto &it = unit_to_id.find(unit_name);
                        if (it == unit_to_id.end()) {
                                unit_to_id[unit_name] = type_id;
                                ti.units.insert(type_id);
                                type_id++;
                        } else {
                                ti.units.insert(it->second);
                        }
                        ti.dimension = string_to_dimension(unit_name);
                }
                ti.source.push_back({SOURCE_INTRINSIC, -1, ""});
                results[ve.variable_name] = ti;
        }

        return results;
}

void do_work(CXCompileCommands cmds, unsigned thread_no, unsigned stride,
             const set<string> &interesting_writes,
             set<string> &functions_with_intrinsic_variables,
             unordered_set<string> &seen_definitions,
             const map<string, string> &type_to_semantic,
             const map<string, map<string, int>> &type_to_field_to_unit,
             vector<map<string, FunctionSummary>> &fn_summaries,
             unordered_map<string, set<unsigned>> &name_to_tu, mutex &lock,
             int num_units, int &file_no, mutex &cout_lock,
             const map<string, TypeInfo> &prior_type_to_typeinfo,
             map<string, TypeInfo> &function_name_to_return_unit_type,
             const map<int, string> &id_to_unitname) {
        unsigned num_cmds = clang_CompileCommands_getSize(cmds);
        CXIndex index = clang_createIndex(0, 0);
        for (auto i = thread_no; i < num_cmds; i += stride) {
                CXCompileCommand cmd =
                    clang_CompileCommands_getCommand(cmds, i);

                // (3 a) print the filename that was compiled by this command
                CXString filename = clang_CompileCommand_getFilename(cmd);
                CXString compile_dir = clang_CompileCommand_getDirectory(cmd);

                cout_lock.lock();
                cout << ++file_no << "/" << num_cmds << " "
                     << clang_getCString(filename) << endl;
                cout_lock.unlock();

                // (3 b) build a translation unit from the compilation command
                // (3 b i) collect arguments
                unsigned num_args = clang_CompileCommand_getNumArgs(cmd);
                vector<CXString> args;
                const char **cmd_argv = new const char *[num_args];
                for (auto j = 0u; j < num_args; j++) {
                        args.push_back(clang_CompileCommand_getArg(cmd, j));
                        cmd_argv[j] = clang_getCString(args[j]);
                }

                // (3 b ii) construct translation unit
                if (change_thread_working_dir(clang_getCString(compile_dir))) {
                        cerr << "[WARN] unable to cd to "
                             << clang_getCString(compile_dir) << ". skipping."
                             << endl;
                        continue;
                }

                CXTranslationUnit unit =
                    clang_createTranslationUnitFromSourceFile(
                        index, nullptr, num_args, cmd_argv, 0, nullptr);

                set<string> possible_frames;
                map<unsigned, set<string>> scope_to_tainted;
                vector<map<string, TypeInfo>> var_types;
                set<string> current_fn_params;
                map<string, int> param_to_number;
                map<int, TypeSourceKind> param_to_typesource_kind;
                map<string, TypeInfo> current_interesting_writes;
                ASTContext ctx = {
                    .types_to_frame_field = type_to_semantic,
                    .type_to_field_to_unit = type_to_field_to_unit,
                    .num_units = num_units,
                    .constraint = UNCONSTRAINED,
                    .possible_frames = possible_frames,
                    .in_mav_constraint = false,
                    .scope_to_tainted = scope_to_tainted,
                    .had_mav_constraint = false,
                    .had_taint = false,
                    .var_types = var_types,
                    .fn_summary = fn_summaries[i],
                    .current_fn = "",
                    .current_usr = "",
                    .current_fn_params = current_fn_params,
                    .param_to_number = param_to_number,
                    .param_to_typesource_kind = param_to_typesource_kind,
                    .total_params = 0,
                    .name_to_tu = name_to_tu,
                    .had_fn_definition = false,
                    .translation_unit_no = i,
                    .semantic_context = "",
                    .writes_to_variables_with_known_types = interesting_writes,
                    .store_to_typeinfo = current_interesting_writes,
                    .functions_with_intrinsic_variables = functions_with_intrinsic_variables,
                    .seen_definitions = seen_definitions,
                    .lock = lock,
                    .thread_no = static_cast<int>(thread_no),
                    .prior_var_to_typeinfo = prior_type_to_typeinfo,
                    .function_names_to_return_unit = function_name_to_return_unit_type,
                    .id_to_unitname = id_to_unitname,
                };
                if (unit) {
                        CXCursor cursor = clang_getTranslationUnitCursor(unit);
                        clang_visitChildren(cursor, ast_walker, &ctx);
                } else {
                        cerr << "[WARN] error building translation unit for "
                             << get_full_path(compile_dir, filename)
                             << ". skipping." << endl;
                }

                // free memory from (3 b ii)
                clang_disposeTranslationUnit(unit);

                // free memory from (3 b i)
                delete[] cmd_argv;
                for (auto str : args)
                        clang_disposeString(str);

                // free memory from (3 b a)
                clang_disposeString(filename);
                clang_disposeString(compile_dir);
        }
        clang_disposeIndex(index);
}

MessageDefinitionType detect_definition_type(const pugi::xml_document &doc) {
        if (doc.child("mavlink")) {
                return MAVLINK;
        }
        if (doc.child("MDM")) {
                return LMCP;
        }
        return UNKNOWN;
}

int main(int argc, char **argv) {
        cxxopts::Options options("sa4u", "static analysis for UAVs");
        // clang-format off
        options.add_options()
          ("c,compilation-database",
           "directory containing the compilation database",
           cxxopts::value<string>())
          ("m,message-definition",
           "path to XML file containing the message spec: Supported specs are "
           "MavLink and LMCP",
           cxxopts::value<string>())
          ("p,prior-types",
           "path to JSON file describing previously known types",
           cxxopts::value<string>())
          ("h,help", 
           "print this message and exit")
          ("v,verbose",
           "enable verbose output");
        // clang-format on

        cxxopts::ParseResult result = options.parse(argc, argv);
        if (result.count("help")) {
                cout << options.help() << endl;
                exit(0);
        }

        string compilation_database_path, message_def_path, prior_types_path;
        try {
                compilation_database_path =
                    result["compilation-database"].as<string>();
                message_def_path = result["message-definition"].as<string>();
                prior_types_path = result["prior-types"].as<string>();
                if (result["verbose"].as<bool>())
                        spdlog::set_level(spdlog::level::trace);
        } catch (domain_error &e) {
                cerr << options.help() << endl;
                exit(1);
        }

        // (0) load data sources
        pugi::xml_document doc;
        ifstream xml_in(message_def_path);
        if (!doc.load(xml_in)) {
                spdlog::critical("cannot load message XML");
                exit(1);
        }
        xml_in.close();

        int num_units = 0;
        map<string, string> type_to_semantic;
        map<string, int> unitname_to_id;
        map<string, map<string, int>> type_to_field_to_unit;
        map<string, TypeInfo> function_to_return_type;

        switch (detect_definition_type(doc)) {
        case MAVLINK:
                type_to_semantic = get_types_to_frame_field(doc);
                type_to_field_to_unit =
                    get_type_to_field_to_unit(doc, unitname_to_id, num_units);
                break;
        case LMCP:
                function_to_return_type = get_units_of_functions(doc, unitname_to_id, num_units);
                break;
        case UNKNOWN:
        default:
                spdlog::critical("message not in a supported spec");
                exit(1);
                break;
        }

        ifstream json_in(prior_types_path);
        if (!json_in) {
                spdlog::critical("cannot load prior JSON");
                exit(1);
        }
        vector<VariableEntry> vars = read_variable_info(json_in);
        json_in.close();
        map<string, TypeInfo> prior_var_to_typeinfo =
            vars_to_typeinfo(vars, unitname_to_id, num_units);

        // Maps the ID of a unit (e.g. 0) to its name (e.g. centimeter). 
        map<int, string> id_to_unitname = invert_map(unitname_to_id);

        // (1) load database
        CXCompilationDatabase_Error err;
        CXCompilationDatabase cdatabase =
            clang_CompilationDatabase_fromDirectory(
                compilation_database_path.c_str(), &err);
        if (err != CXCompilationDatabase_NoError) {
                spdlog::critical("cannot load compilation database");
                exit(1);
        }

        // (2) get compilation commands
        CXCompileCommands cmds =
            clang_CompilationDatabase_getAllCompileCommands(cdatabase);

        // (3) search each file in the compilation commands for mavlink messages
        unsigned num_cmds = clang_CompileCommands_getSize(cmds);
        vector<map<string, FunctionSummary>> fn_summaries(num_cmds);
        unordered_map<string, set<unsigned>> name_to_tu;
        name_to_tu.reserve(num_cmds * 50);

        set<string> interesting_writes;
        for (const auto &entry : vars)
                interesting_writes.insert(entry.variable_name);

        set<string> functions_with_intrinsic_variables;
        unordered_set<string> seen_definitions;
        seen_definitions.reserve(num_cmds * 50);

        // initialize worker threads
        int file_no = 0;
        mutex lock, cout_lock;
        vector<thread> workers;
        unsigned num_workers = max(1u, thread::hardware_concurrency());
        for (unsigned i = 0u; i < num_workers; i++) {
                workers.push_back(thread(
                    do_work, cmds, i, num_workers, ref(interesting_writes),
                    ref(functions_with_intrinsic_variables),
                    ref(seen_definitions), ref(type_to_semantic),
                    ref(type_to_field_to_unit), ref(fn_summaries),
                    ref(name_to_tu), ref(lock), num_units, ref(file_no),
                    ref(cout_lock), ref(prior_var_to_typeinfo), 
                    ref(function_to_return_type), ref(id_to_unitname)));
        }

        // wait for completion
        for (auto &thread : workers)
                thread.join();

        clang_CompileCommands_dispose(cmds);
        clang_CompilationDatabase_dispose(cdatabase);

        vector<vector<string>> traces = get_unconstrained_traces(
            name_to_tu, fn_summaries, functions_with_intrinsic_variables,
            prior_var_to_typeinfo, num_units);
        
        cout << "===DIAGNOSTICS===" << endl;
        cout << "functions with intrinsic variables: " << endl;
        for (const string &str : functions_with_intrinsic_variables) {
                cout << str << endl;
        }

        // for (const auto &trace: traces) {
        //         string sep = "";
        //         for (const auto &fn: trace) {
        //                 cout << sep << fn;
        //                 sep = " -> ";
        //         }
        //         cout
        exit(0);
}
