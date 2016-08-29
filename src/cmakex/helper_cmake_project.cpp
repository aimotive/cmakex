#include "helper_cmake_project.h"

#include <nowide/cstdio.hpp>

#include "cmakex-types.h"
#include "cmakex_utils.h"
#include "filesystem.h"
#include "misc_utils.h"
#include "out_err_messages.h"
#include "print.h"

namespace cmakex {

namespace fs = filesystem;

const char* k_build_script_add_pkg_out_filename = "add_pkg_out.txt";
const char* k_build_script_cmakex_out_filename = "cmakex_out.txt";
const char* k_default_binary_dirname = "b";
const char* k_executor_project_command_cache_var = "__CMAKEX_EXECUTOR_PROJECT_COMMAND";
const char* k_build_script_executor_log_name = "deps_script_wrapper";

string deps_script_wrapper_cmakelists()
{
    return stringf(
               "cmake_minimum_required(VERSION ${CMAKE_VERSION})\n\n"
               "if(DEFINED %s)\n"
               "    set(command \"${%s}\")\n"
               "    unset(%s CACHE)\n"
               "endif()\n\n",
               k_executor_project_command_cache_var, k_executor_project_command_cache_var,
               k_executor_project_command_cache_var) +

           "function(add_pkg NAME)\n"
           "  # test list compatibility\n"
           "  set(s ${NAME})\n"
           "  list(LENGTH s l)\n"
           "  if (NOT l EQUAL 1)\n"
           "    message(FATAL_ERROR \"\\\"${NAME}\\\" is an invalid name for a package\")\n"
           "  endif()\n"
           //"  message(STATUS \"file(APPEND \\\"${__CMAKEX_ADD_PKG_OUT}\\\" "
           //"\\\"${NAME};${ARGN}\\\\n\\\")\")\n"
           "  set(line \"${NAME}\")\n"
           "  foreach(x IN LISTS ARGN)\n"
           "    set(line \"${line}\\t${x}\")\n"
           "  endforeach()\n"
           "  file(APPEND \"${__CMAKEX_ADD_PKG_OUT}\" \"${line}\\n\")\n"
           "endfunction()\n\n"

           "# include deps script within a function to protect local variables\n"
           "function(include_deps_script path)\n"
           "  if(NOT IS_ABSOLUTE \"${path}\")\n"
           "    set(path \"${CMAKE_CURRENT_LIST_DIR}/${path}\")\n"
           "  endif()\n"
           "  if(NOT EXISTS \"${path}\")\n"
           "    message(FATAL_ERROR \"Dependency script not found: \\\"${path}\\\".\")\n"
           "  endif()\n"
           "  include(\"${path}\")\n"
           "endfunction()\n\n"

           "if(DEFINED command)\n"
           "  message(STATUS \"Dependency script wrapper command: ${command}\")\n"
           "  list(GET command 0 verb)\n\n"
           "  if(verb STREQUAL \"run\")\n"
           "    list(LENGTH command l)\n"
           "    if(NOT l EQUAL 3)\n"
           "      message(FATAL_ERROR \"Internal error, invalid command\")\n"
           "    endif()\n"
           "    list(GET command 1 path)\n"
           "    list(GET command 2 out)\n"
           "    if(NOT EXISTS \"${out}\" OR IS_DIRECTORY \"${out}\")\n"
           "      message(FATAL_ERROR \"Internal error, the output file "
           "\\\"${out}\\\" is not an existing file.\")\n"
           "    endif()\n"
           "    set(__CMAKEX_ADD_PKG_OUT \"${out}\")\n"
           "    include_deps_script(\"${path}\")\n"
           "  endif()\n"
           "else()\n"
           "    message(STATUS \"No command specified.\")\n"
           "endif()\n\n";
}
string deps_script_wrapper_cmakelists_checksum(const std::string& x)
{
    auto hs = std::to_string(std::hash<std::string>{}(x));
    return stringf("# script hash: %s", hs.c_str());
}

HelperCmakeProject::HelperCmakeProject(string_par binary_dir)
    : binary_dir(fs::absolute(binary_dir.c_str()).string()),
      cfg(binary_dir),
      build_script_executor_binary_dir(cfg.cmakex_executor_dir() + "/" + k_default_binary_dirname),
      build_script_add_pkg_out_file(cfg.cmakex_tmp_dir() + "/" +
                                    k_build_script_add_pkg_out_filename),
      build_script_cmakex_out_file(cfg.cmakex_tmp_dir() + "/" + k_build_script_cmakex_out_filename)
{
}

CMakeCacheTracker::report_t HelperCmakeProject::configure(const vector<string>& global_cmake_args)
{
    for (auto d : {cfg.cmakex_executor_dir(), cfg.cmakex_tmp_dir()}) {
        if (!fs::is_directory(d)) {
            string msg;
            try {
                fs::create_directories(d);
            } catch (const exception& e) {
                msg = e.what();
            } catch (...) {
                msg = "unknown exception";
            }
            if (!msg.empty())
                throwf("Can't create directory \"%s\", reason: %s.", d.c_str(), msg.c_str());
        }
    }

    // create the text of CMakelists.txt
    const string cmakelists_text = deps_script_wrapper_cmakelists();
    const string cmakelists_text_hash = deps_script_wrapper_cmakelists_checksum(cmakelists_text);

    // force write if not exists or first hash line differs
    string cmakelists_path = cfg.cmakex_executor_dir() + "/CMakeLists.txt";
    bool cmakelists_exists = fs::exists(cmakelists_path);
    if (cmakelists_exists) {
        cmakelists_exists = false;  // if anything goes wrong, pretend it doesn't exist
        do {
            FILE* f = nowide::fopen(cmakelists_path.c_str(), "r");
            if (!f)
                break;
            const int c_bufsize = 128;
            char buf[c_bufsize];
            buf[0] = 0;
            auto r = fgets(buf, c_bufsize, f);
            (void)r;  // does not count, the hash will decide in the next line
            cmakelists_exists = strncmp(buf, cmakelists_text_hash.c_str(), c_bufsize) == 0;
            fclose(f);
        } while (false);
    }
    if (!cmakelists_exists) {
        auto f = must_fopen(cmakelists_path.c_str(), "w");
        must_fprintf(f, "%s\n%s\n", cmakelists_text_hash.c_str(), cmakelists_text.c_str());
    }

    vector<string> args;
    args.emplace_back(string("-H") + cfg.cmakex_executor_dir());
    args.emplace_back(string("-B") + build_script_executor_binary_dir);

    fs::create_directories(build_script_executor_binary_dir);
    CMakeCacheTracker cvt(build_script_executor_binary_dir);

    {
        auto t = cvt.about_to_configure(global_cmake_args, true);
        auto& diff_global_cmake_args = get<0>(t);
        args.insert(args.end(), BEGINEND(diff_global_cmake_args));
    }

    args.emplace_back(string("-U") + k_executor_project_command_cache_var);
    log_exec("cmake", args);
    OutErrMessagesBuilder oeb(pipe_capture, pipe_capture);
    int r = exec_process("cmake", args, oeb.stdout_callback(), oeb.stderr_callback());
    auto oem = oeb.move_result();

    save_log_from_oem("CMake-configure", r, oem, cfg.cmakex_log_dir(),
                      string(k_build_script_executor_log_name) + "-configure" + k_log_extension);

    if (r != EXIT_SUCCESS)
        throwf("Failed configuring dependency script wrapper project, result: %d.", r);

    return cvt.cmake_config_ok();
}

// todo detect if there's cmake and git and provide better error message
vector<string> HelperCmakeProject::run_deps_script(string_par deps_script_file)
{
    vector<string> args;
    args.emplace_back(build_script_executor_binary_dir);

    // create empty add_pkg out file
    {
        auto f = must_fopen(build_script_add_pkg_out_file.c_str(), "w");
    }
    args.emplace_back(string("-D") + k_executor_project_command_cache_var + "=run;" +
                      deps_script_file.c_str() + ";" + build_script_add_pkg_out_file);

    log_exec("cmake", args);
    OutErrMessagesBuilder oeb2(pipe_capture, pipe_capture);
    int r = exec_process("cmake", args, oeb2.stdout_callback(), oeb2.stderr_callback());
    auto oem2 = oeb2.move_result();
    save_log_from_oem("Dependency script", r, oem2, cfg.cmakex_log_dir(),
                      string(k_build_script_executor_log_name) + "-run" + k_log_extension);
    if (r != EXIT_SUCCESS)
        throwf("Failed executing dependency script wrapper, result: %d.", r);

    // read the add_pkg_out
    return must_read_file_as_lines(build_script_add_pkg_out_file);
}
}