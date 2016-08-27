#include "build.h"

#include <adasworks/sx/check.h>

#include "cmakex_utils.h"
#include "filesystem.h"
#include "installdb.h"
#include "misc_utils.h"
#include "out_err_messages.h"
#include "print.h"

namespace cmakex {

namespace fs = filesystem;

void build(string_par binary_dir,
           string_par pkg_name,
           string_par pkg_source_dir,
           const vector<string>& cmake_args_in,
           config_name_t config,
           const vector<string>& build_targets,
           bool force_config_step)
{
    cmakex_config_t cfg(binary_dir);
    CHECK(cfg.cmakex_cache().valid);

    // cmake-configure step: Need to do it if
    //
    // force_config_step || new_binary_dir || cmakex_cache_tracker indicates changed settings

    string source_dir;
    string pkg_bin_dir_of_config;
    vector<string> cmake_args = cmake_args_in;

    if (pkg_name.empty()) {           // main project
        source_dir = pkg_source_dir;  // cwd-relative or absolute
        pkg_bin_dir_of_config =
            cfg.main_binary_dir_of_config(config, cfg.cmakex_cache().per_config_bin_dirs);
    } else {
        source_dir = cfg.pkg_clone_dir(pkg_name);
        if (!pkg_source_dir.empty())
            source_dir += "/" + pkg_source_dir.str();
        pkg_bin_dir_of_config =
            cfg.pkg_binary_dir_of_config(pkg_name, config, cfg.cmakex_cache().per_config_bin_dirs);

        // check if there's no install_prefix
        for (auto& c : cmake_args) {
            if (!starts_with(c, "-DCMAKE_INSTALL_PREFIX"))
                continue;
            auto pca = parse_cmake_arg(c);
            if (pca.name == "CMAKE_INSTALL_PREFIX") {
                throwf(
                    "Internal error: global and package cmake_args should not change "
                    "CMAKE_INSTALL_PREFIX: '%s'",
                    c.c_str());
            }
        }
        cmake_args.emplace_back(
            stringf("-DCMAKE_INSTALL_PREFIX=%s", cfg.deps_install_dir().c_str()));
    }

    if (!cfg.cmakex_cache().multiconfig_generator) {
        if (config.is_noconfig())
            cmake_args.emplace_back(stringf("-UCMAKE_BUILD_TYPE"));
        else
            cmake_args.emplace_back(
                stringf("-DCMAKE_BUILD_TYPE=%s", config.get_prefer_empty().c_str()));
    }

    force_config_step =
        force_config_step || !fs::is_regular_file(pkg_bin_dir_of_config + "/CMakeCache.txt");

    {  // scope only
        CMakeCacheTracker cct(pkg_bin_dir_of_config);
        vector<string> cmake_args_to_apply;
        if (cfg.cmakex_cache().per_config_bin_dirs) {
            // apply and confirm args here and use it for new bin dirs
            string bin_dir_common = pkg_name.empty() ? cfg.main_binary_dir_common()
                                                     : cfg.pkg_binary_dir_common(pkg_name);
            update_reference_cmake_cache_tracker(bin_dir_common, cmake_args);
            cmake_args_to_apply =
                cct.about_to_configure(cmake_args, force_config_step, bin_dir_common);
        } else
            cmake_args_to_apply = cct.about_to_configure(cmake_args, force_config_step);

        // do config step only if needed
        if (force_config_step || !cmake_args_to_apply.empty()) {
            cmake_args_to_apply.insert(
                cmake_args_to_apply.begin(),
                {string("-H") + source_dir, string("-B") + pkg_bin_dir_of_config});

            log_exec("cmake", cmake_args_to_apply);

            int r;
            if (pkg_name.empty()) {
                r = exec_process("cmake", cmake_args_to_apply);
            } else {
                OutErrMessagesBuilder oeb(pipe_capture, pipe_capture);
                r = exec_process("cmake", cmake_args_to_apply, oeb.stdout_callback(),
                                 oeb.stderr_callback());
                auto oem = oeb.move_result();

                save_log_from_oem("CMake-configure", r, oem, cfg.cmakex_log_dir(),
                                  stringf("%s-%s-configure%s", pkg_name.c_str(),
                                          config.get_prefer_NoConfig().c_str(), k_log_extension));
            }
            if (r != EXIT_SUCCESS)
                throwf("CMake configure step failed, result: %d.", r);

            // if cmake_args_to_apply is empty then this may not be executed
            // but if it's empty there are no pending variables so the cmake_config_ok
            // will not be missing
            cct.cmake_config_ok();
        }
    }

    for (auto& target : build_targets) {
        vector<string> args{{"--build", pkg_bin_dir_of_config}};
        if (!target.empty()) {
            args.insert(args.end(), {"--target", target.c_str()});
        }

        if (cfg.cmakex_cache().multiconfig_generator) {
            CHECK(!config.is_noconfig());
            args.insert(args.end(), {"--config", config.get_prefer_NoConfig().c_str()});
        }

        // todo add build_args
        // todo add native tool args
        // todo clear install dir if --clean-first

        log_exec("cmake", args);
        {  // scope only
            int r;
            if (pkg_name.empty()) {
                r = exec_process("cmake", args);
            } else {
                OutErrMessagesBuilder oeb(pipe_capture, pipe_capture);
                r = exec_process("cmake", args, oeb.stdout_callback(), oeb.stderr_callback());
                auto oem = oeb.move_result();

                save_log_from_oem(
                    "Build", r, oem, cfg.cmakex_log_dir(),
                    stringf("%s-%s-build-%s%s", pkg_name.c_str(),
                            config.get_prefer_NoConfig().c_str(),
                            target.empty() ? "all" : target.c_str(), k_log_extension));
            }
            if (r != EXIT_SUCCESS)
                throwf("CMake build step failed, result: %d.", r);
        }
    }  // for targets

// todo
#if 0
        // test step
        if (pars.flag_t) {
            auto tic = high_resolution_clock::now();
            string step_string =
                stringf("test step%s", config.empty() ? "" : (string(" (") + config + ")").c_str());
            log_info("Begin %s", step_string.c_str());
            fs::current_path(pars.binary_dir);
            vector<string> args;
            if (!config.empty()) {
                args.emplace_back("-C");
                args.emplace_back(config);
            }
            log_exec("ctest", args);
            int r = exec_process("ctest", args);
            if (r != EXIT_SUCCESS)
                throwf("Testing failed with error code %d", r);
            log_info(
                "End %s, elapsed %s", step_string.c_str(),
                sx::format_duration(dur_sec(high_resolution_clock::now() - tic).count()).c_str());
        }
#endif
}
}
