/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2015 Microsoft Corporation
 * 
 * -=- Robust Distributed System Nucleus (rDSN) -=- 
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/*
 * Description:
 *     What is this file about?
 *
 * Revision history:
 *     xxxx-xx-xx, author, first version
 *     xxxx-xx-xx, author, fix bug about xxx
 */


# include "daemon.server.h"
# include "daemon.h"
# include <dsn/cpp/utils.h>
# include <dsn/tool_api.h>
# include <dsn/utility/module_init.cpp.h>

# if defined(__linux__)
# include <sys/prctl.h>
# endif

using namespace ::dsn::replication;
extern void daemon_register_test_server();

MODULE_INIT_BEGIN(daemon)
    dsn::register_app< ::dsn::dist::daemon>("daemon");
    daemon_register_test_server();
MODULE_INIT_END

namespace dsn
{
    namespace dist
    {
# if defined(__linux__)
        static daemon_s_service* s_single_daemon = nullptr;
        void daemon_s_service::on_exit(::dsn::sys_exit_type st)
        {
            int pid = getpid();
            kill(-pid, SIGTERM);
            sleep(2);
            kill(-pid, SIGKILL);
        }
# endif

        daemon_s_service::daemon_s_service() 
            : ::dsn::serverlet<daemon_s_service>("daemon_s"), _online(false)
        {
# if defined(__linux__)
            s_single_daemon = this;
            ::dsn::tools::sys_exit.put_back(daemon_s_service::on_exit, "daemon.exit");
            setpgid(0, 0);
# endif

            _working_dir = utils::filesystem::path_combine(dsn_get_app_data_dir(), "apps");
            _package_server = rpc_address(
                dsn_config_get_value_string("apps.daemon", "package_server_host", "", "the host name of the app store where to download package"),
                (uint16_t)dsn_config_get_value_uint64("apps.daemon", "package_server_port", 26788, "the port of the app store where to download package")
                );
            _package_dir_on_package_server = dsn_config_get_value_string("apps.daemon", "package_dir", "", "the dir on the app store where to download package");
            _app_port_min = (uint32_t)dsn_config_get_value_uint64("apps.daemon", "app_port_min", 59001, "the minimum port that can be assigned to app");
            _app_port_max = (uint32_t)dsn_config_get_value_uint64("apps.daemon", "app_port_max", 60001, "the maximum port that can be assigned to app");
            _config_sync_interval_seconds = (uint32_t)dsn_config_get_value_uint64("apps.daemon", "config_sync_interval_seconds", 5, "sync configuration with meta server for every how many seconds");

# ifdef _WIN32
            _unzip_format_string = dsn_config_get_value_string(
                "apps.daemon", 
                "unzip_format_string", 
                "unzip -qo %s.zip -d %s", 
                "unzip cmd used as fprintf(fmt-str, src-package-name, dst-dir)"
            );
# else
            _unzip_format_string = dsn_config_get_value_string(
                "apps.daemon",
                "unzip_format_string",
                "tar zxvf %s.tar.gz -C %s",
                "unzip cmd used as fprintf(fmt-str, src-package-name, dst-dir)"
            );
# endif

            if (!utils::filesystem::directory_exists(_working_dir))
            {
                utils::filesystem::create_directory(_working_dir);
            }

# ifdef _WIN32
            _job = CreateJobObjectA(NULL, NULL);
            dassert(_job != NULL, "create windows job failed, err = %d", ::GetLastError());

            JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = { 0 };

            // Configure all child processes associated with the job to terminate when the
            jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

            if (0 == SetInformationJobObject(_job, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli)))
            {
                dassert(false, "Could not SetInformationJobObject, err = %d", ::GetLastError());
            }
# else
# endif
        }

        daemon_s_service::~daemon_s_service()
        {
        }

        void daemon_s_service::on_config_proposal(const ::dsn::replication::configuration_update_request& proposal)
        {
            dassert(proposal.info.is_stateful == false, 
                "stateful replication not supported by daemon, please using a different layer2 handler");

            switch (proposal.type)
            {
            case config_type::CT_ADD_SECONDARY:
            case config_type::CT_ADD_SECONDARY_FOR_LB:
                on_add_app(proposal);
                break;

            case config_type::CT_REMOVE:
                on_remove_app(proposal);
                break;

            default:
                dwarn("not supported configuration type %s received", enum_to_string(proposal.type));
                break;
            }
        }

        DEFINE_TASK_CODE(LPC_DAEMON_APPS_CHECK_TIMER, TASK_PRIORITY_COMMON, THREAD_POOL_DEFAULT)

        void daemon_s_service::open_service()
        {
            std::vector<rpc_address> meta_servers;
            dsn::replication::replica_helper::load_meta_servers(meta_servers);
            _fd.reset(new slave_failure_detector_with_multimaster(meta_servers, 
                [=]() { this->on_master_disconnected(); },
                [=]() { this->on_master_connected(); }
                ));

            this->register_rpc_handler(RPC_CONFIG_PROPOSAL, "config_proposal", &daemon_s_service::on_config_proposal);

            auto err = _fd->start(
                5,
                3,
                14,
                15
                );

            dassert(ERR_OK == err, "failure detector start failed, err = %s", err.to_string());

            _fd->register_master(_fd->current_server_contact());

            _app_check_timer = tasking::enqueue_timer(
                LPC_DAEMON_APPS_CHECK_TIMER,
                this,
                [this]{ this->check_apps(); },
                std::chrono::milliseconds(
                    (int)dsn_config_get_value_uint64("apps.daemon", "local_app_check_period_ms", 
                        500, 
                        "periodically check local apps' healthy status with this period")
                        )
                );


            _config_sync_timer = tasking::enqueue_timer(
                LPC_QUERY_CONFIGURATION_ALL,
                this,
                [this] {
                query_configuration_by_node();
                },
                std::chrono::seconds(_config_sync_interval_seconds)
                );

            _cli_kill_partition = dsn_cli_app_register(
                "kill_partition",
                "kill_partition app_id partition_index",
                "kill partition with its global partition id",
                (void*)this,
                [](void *context, int argc, const char **argv, dsn_cli_reply *reply)
                {
                    auto this_ = (daemon_s_service*)context;
                    this_->on_kill_app_cli(context, argc, argv, reply);
                },
                [](dsn_cli_reply reply)
                {
                    std::string* s = (std::string*)reply.context;
                    delete s;
                }
                );

        }

        void daemon_s_service::query_configuration_by_node()
        {
            if (!_online)
            {
                return;
            }

            dsn_message_t msg = dsn_msg_create_request(RPC_CM_QUERY_NODE_PARTITIONS);

            configuration_query_by_node_request req;
            req.node = primary_address();
            ::dsn::marshall(msg, req);

            rpc_address target(_fd->get_servers());
            rpc::call(
                target,
                msg,
                this,
                [this](error_code err, dsn_message_t request, dsn_message_t resp)
                {
                    on_node_query_reply(err, request, resp);
                }
            );
        }

        void daemon_s_service::on_node_query_reply(error_code err, dsn_message_t request, dsn_message_t response)
        {
            ddebug(
                "%s: node view replied, err = %s",
                primary_address().to_string(),
                err.to_string()
                );

            if (err != ERR_OK)
            {
                // retry when the timer fires again later
                return;
            }
            else
            {
                if (!_online)
                    return;

                configuration_query_by_node_response resp;
                ::dsn::unmarshall(response, resp);

                if (resp.err != ERR_OK || resp.err == ERR_BUSY)
                    return;

                std::unordered_map<dsn::gpid, std::shared_ptr< app_internal> > apps;

                _lock.lock_read();
                for (auto& pkg : _apps)
                {
                    for (auto& app : pkg.second->apps)
                    {
                        apps.emplace(app.second->configuration.pid, app.second);
                    }
                }
                _lock.unlock_read();

                
                // find apps on meta server but not on local daemon
                rpc_address host = primary_address();
                for (auto appc : resp.partitions)
                {
                    int i;
                    for (i = 0; i < (int)appc.config.secondaries.size(); i++)
                    {
                        // host nodes stored in secondaries
                        if (appc.config.secondaries[i] == host)
                        {
                            // worker nodes stored in last-drops
                            break;
                        }
                    }

                    dassert(i < (int)appc.config.secondaries.size(),
                        "host address %s must exist in secondary list of partition %d.%d",
                        host.to_string(), appc.config.pid.get_app_id(), appc.config.pid.get_partition_index()
                    );
                    
                    bool found = false;
                    auto it = apps.find(appc.config.pid);
                    if (it != apps.end())
                    {
                        found = true;
                        apps.erase(it);
                    }

                    if (!found)
                    {
                        configuration_update_request req;
                        req.info = appc.info;
                        req.config = appc.config;
                        req.host_node = host;
                        req.type = config_type::CT_REMOVE;

                        // worker nodes stored in last-drops
                        req.node = appc.config.last_drops[i];

                        std::shared_ptr<app_internal> app(new app_internal(req));
                        app->exited = true;
                        app->working_port = req.node.port();

                        update_configuration_on_meta_server(config_type::CT_REMOVE, std::move(app));
                    }
                }

                // find apps on local daemon but not on meta server
                for (auto& app : apps)
                {
                    auto cap = app.second;
                    kill_app(std::move(cap));
                }
            }
        }

        void daemon_s_service::on_kill_app_cli(void *context, int argc, const char **argv, dsn_cli_reply *reply)
        {
            error_code err = ERR_INVALID_PARAMETERS;
            if (argc >= 2)
            {
                gpid gpid;
                gpid.set_app_id(atoi(argv[0]));
                gpid.set_partition_index(atoi(argv[1]));
                std::shared_ptr<app_internal> app = nullptr;
                {
                    ::dsn::service::zauto_write_lock l(_lock);
                    for (auto& pkg : _apps)
                    {
                        auto it = pkg.second->apps.find(gpid);
                        if (it != pkg.second->apps.end())
                        {
                            app = it->second;
                            break;
                        }
                    }
                }

                if (app == nullptr)
                {
                    err = ERR_OBJECT_NOT_FOUND;
                }
                else
                {
                    kill_app(std::move(app));
                    err = ERR_OK;
                }
            }

            std::string* resp_json = new std::string();
            *resp_json = err.to_string();
            reply->context = resp_json;
            reply->message = (const char*)resp_json->c_str();
            reply->size = resp_json->size();
            return;
        }

        void daemon_s_service::close_service()
        {
            _app_check_timer->cancel(true);
            _fd->stop();
            this->unregister_rpc_handler(RPC_CONFIG_PROPOSAL);

            dsn_cli_deregister(_cli_kill_partition);
            _cli_kill_partition = nullptr;
        }

        void daemon_s_service::on_master_connected()
        {
            _online = true;
            dinfo("master is connected");
        }
        
        void daemon_s_service::on_master_disconnected()
        {       
            _online = false;

            // TODO: fail-over
           /* {
                ::dsn::service::zauto_read_lock l(_lock);
                for (auto& app : _apps)
                {
                    kill_app(std::move(app.second));
                }

                _apps.clear();
            }*/

            dinfo("master is disconnected");
        }

        DEFINE_TASK_CODE_AIO(LPC_DAEMON_DOWNLOAD_PACKAGE, TASK_PRIORITY_COMMON, THREAD_POOL_DEFAULT)

        void daemon_s_service::on_add_app(const ::dsn::replication::configuration_update_request & proposal)
        {
            std::shared_ptr<package_internal> ppackage;
            std::shared_ptr<app_internal> old_app, app;
            bool resource_ready = false;
            bool is_resource_downloading = false;

            {
                ::dsn::service::zauto_write_lock l(_lock);

                // check package exists or not
                {
                    auto it = _apps.find(proposal.info.app_type);

                    // app is running with the same package
                    if (it != _apps.end())
                    {
                        ppackage = it->second;
                    }
                    else
                    {
                        ppackage.reset(new package_internal());
                        ppackage->package_dir = utils::filesystem::path_combine(_working_dir, proposal.info.app_type);

                        // each package is required with a config.ini in it for run as: dsn.svchost config.ini -cargs port=%port%;envs
                        ppackage->config_file = utils::filesystem::path_combine(ppackage->package_dir, "config.ini");

                        _apps.emplace(proposal.info.app_type, ppackage);
                    }
                }

                // check app exists or not
                {
                    resource_ready = ppackage->resource_ready;
                    is_resource_downloading = ppackage->downloading;
                    if (!is_resource_downloading)
                        ppackage->downloading = true; // done later in this call

                    auto it = ppackage->apps.find(proposal.config.pid);

                    // app is running with the same package
                    if (it != ppackage->apps.end())
                    {
                        // proposal's package is older or the same
                        // ballot is the package version for stateless applications
                        if (proposal.config.ballot <= it->second->configuration.ballot)
                            return;

                        else
                        {
                            old_app = std::move(it->second);
                            it->second.reset(new app_internal(proposal));
                            app = it->second;
                        }
                    }
                    else
                    {
                        app.reset(new app_internal(proposal));
                        ppackage->apps.emplace(proposal.config.pid, app);
                    }
                }
            }
                
            // kill old app if necessary
            if (nullptr != old_app)
            {
                kill_app(std::move(old_app));
            }
            
            // check and start
            if (resource_ready)
            {                
                start_app(std::move(app), std::move(ppackage));
            }

            // download package first if necesary
            else if (!is_resource_downloading)
            {
                // TODO: better way to download package from app store 
# ifdef _WIN32
                std::vector<std::string> files{ proposal.info.app_type + ".zip" };
# else
                std::vector<std::string> files{ proposal.info.app_type + ".tar.gz" };
# endif

                dinfo("start downloading package %s from %s to %s",
                    proposal.info.app_type.c_str(),
                    _package_server.to_string(),
                    _working_dir.c_str()
                    );

                file::copy_remote_files(
                    _package_server,
                    _package_dir_on_package_server,
                    files,
                    _working_dir,
                    true,
                    LPC_DAEMON_DOWNLOAD_PACKAGE,
                    this,
                    [this, pkg = std::move(ppackage), capp = std::move(app)](error_code err, size_t sz) mutable
                    {
                        if (err == ERR_OK)
                        {
                            // TODO: using zip lib instead
                            char command[1024];
                            snprintf_p(command, sizeof(command), 
                                _unzip_format_string.c_str(),
                                (_working_dir + '/' + capp->info.app_type).c_str(),
                                _working_dir.c_str()
                                );
                            
                            // decompress when completed
                            int serr = system(command);
                            if (serr != 0)
                            {
                                derror("extract package %s with cmd '%s' failed, err = %d",
                                    pkg->package_dir.c_str(),
                                    command,
                                    serr
                                );

                                err = ERR_UNKNOWN;

                                utils::filesystem::remove_path(pkg->package_dir);
                                {
                                    ::dsn::service::zauto_write_lock l(_lock);
                                    _apps.erase(capp->info.app_type);
                                }
                            }
                            else
                            {
                                if (utils::filesystem::file_exists(pkg->config_file))
                                {
                                    same_package_apps apps;
                                    {
                                        ::dsn::service::zauto_write_lock l(_lock);
                                        apps = pkg->apps;
                                        pkg->resource_ready = true;
                                        pkg->downloading = false;
                                    }

                                    for (auto& app : apps)
                                    {
                                        auto pkg2 = pkg;
                                        start_app(std::move(app.second), std::move(pkg2));
                                    }
                                }
                                else
                                {
                                    derror("package %s does not contain config file '%s' in it",
                                        pkg->package_dir.c_str(),
                                        pkg->config_file.c_str()
                                    );

                                    err = ERR_OBJECT_NOT_FOUND;

                                    utils::filesystem::remove_path(pkg->package_dir);
                                    {
                                        ::dsn::service::zauto_write_lock l(_lock);
                                        _apps.erase(capp->info.app_type);
                                    }
                                }
                            }
                        }
                        else
                        {

                            derror("download app %s failed, err = %s ...",
                                capp->info.app_type.c_str(),
                                err.to_string()
                            );

                            utils::filesystem::remove_path(pkg->package_dir);
                            {
                                ::dsn::service::zauto_write_lock l(_lock);

                                // TODO: try multiple times for timeouts
                                _apps.erase(capp->info.app_type);
                            }
                        }
                    }
                    );
            }
        }

        void daemon_s_service::on_remove_app(const ::dsn::replication::configuration_update_request & proposal)
        {
            // check app exists or not
            std::shared_ptr<app_internal> app;

            {
                ::dsn::service::zauto_read_lock l(_lock);
                auto it = _apps.find(proposal.info.app_type);

                // app is running with the same package
                if (it != _apps.end())
                {
                    auto it2 = it->second->apps.find(proposal.config.pid);

                    if (it2 != it->second->apps.end())
                    {
                        // proposal's package is older or the same
                        // ballot is the package version for stateless applications
                        if (proposal.config.ballot <= it2->second->configuration.ballot)
                            return;
                        else
                        {
                            app = std::move(it2->second);
                            it->second->apps.erase(it2);
                            if (it->second->apps.empty())
                            {
                                _apps.erase(proposal.info.app_type);
                            }
                        }
                    }
                }
            }

            if (nullptr != app)
            {
                auto cap_app = app;
                kill_app(std::move(cap_app));

                update_configuration_on_meta_server(config_type::CT_REMOVE, std::move(app));
            }
        }

        void daemon_s_service::start_app(std::shared_ptr<app_internal> && app, std::shared_ptr<package_internal>&& pkg)
        {
            dassert(nullptr == app->process_handle, "app handle must be empty at this point");

            // set port and run
            for (int i = 0; i < 10; i++)
            {
                uint32_t port = (uint32_t)dsn_random32(_app_port_min, _app_port_max);

                // set up working dir as _working_dir/package-id/gpid.port
                {
                    std::stringstream ss;
                    ss << app->configuration.pid.get_app_id() << "." << app->configuration.pid.get_partition_index() << "." << port;
                    app->working_dir = utils::filesystem::path_combine(pkg->package_dir, ss.str());

                    if (utils::filesystem::directory_exists(app->working_dir))
                        continue;

                    utils::filesystem::create_directory(app->working_dir);
                }

                std::stringstream ss;
# ifdef _WIN32
                ss << "dsn.svchost.exe ";
# else
                ss << "cd " << app->working_dir << " && dsn.svchost ";
# endif
                ss << pkg->config_file << " -cargs port=" << port << ";";
                for (auto& kv : app->info.envs)
                {
                    ss << kv.first << "=" << kv.second << ";";
                }
                
                std::string command = ss.str();

                app->working_port = port;

                dinfo("try start app %s with command %s at working dir %s ...",
                    app->info.app_type.c_str(),
                    command.c_str(),
                    app->working_dir.c_str()
                    );
# ifdef _WIN32
                STARTUPINFOA si;
                PROCESS_INFORMATION pi;

                ZeroMemory(&si, sizeof(si));
                si.cb = sizeof(si);
                ZeroMemory(&pi, sizeof(pi));

                if (::CreateProcessA(NULL, (LPSTR)command.c_str(), NULL, NULL, TRUE, CREATE_NEW_CONSOLE, 
                    NULL, // env
                    (LPSTR)app->working_dir.c_str(), &si, &pi))
                {
                    if (0 == ::AssignProcessToJobObject(_job, pi.hProcess))
                    {
                        // dassert(false, "cannot attach process to job, err = %d", ::GetLastError());
                    }

                    CloseHandle(pi.hThread);                    
                }
                else
                {
                    derror("create process failed, err = %d", ::GetLastError());
                    kill_app(std::move(app));
                    return;
                }

                // sleep a while to see whether the port is usable
                if (WAIT_TIMEOUT == ::WaitForSingleObject(pi.hProcess, 50))
                {
                    app->process_handle = pi.hProcess;
                    break;
                }
                else
                {
                    ::CloseHandle(pi.hProcess);
                }
# else
                int child = fork();
                if (-1 == child)
                    break;

                // child process
                else if (child == 0)
                {
                    // run command
                    char* const argv[] = { (char*)"sh", (char*)"-c", (char*)command.c_str(), nullptr };
                    execve("/bin/sh", argv, environ);
                    exit(0);
                }
                
                // daemon
                // wait for a while
                std::this_thread::sleep_for(std::chrono::milliseconds(50));

                // see if the process is still there
                if (getpgid(child) >= 0)
                {
                    app->process_handle = (dsn_handle_t)(uint64_t)child;
                    break;
                }

                // not exsit, try again
                else
                {
                    continue;
                }
# endif
            }

            // register to meta server if successful
            if (!app->exited && app->process_handle)
            {
                update_configuration_on_meta_server(config_type::CT_ADD_SECONDARY, std::move(app));
            }

            // remove for failure too many times
            else
            {
                kill_app(std::move(app));
            }
        }
        
        void daemon_s_service::kill_app(std::shared_ptr<app_internal> && app)
        {
            dinfo("kill app %s at working dir %s, port %d",
                app->info.app_type.c_str(),
                app->working_dir.c_str(),
                (int)app->working_port
                );

            {
                ::dsn::service::zauto_write_lock l(_lock);
                auto it = _apps.find(app->info.app_type);
                if (it != _apps.end())
                {
                    it->second->apps.erase(app->configuration.pid);
                    if (it->second->apps.empty())
                        _apps.erase(it);
                }
            }

            if (!app->exited && app->process_handle)
            {
# ifdef _WIN32
                std::ostringstream pid;
                std::string command;
                ::TerminateProcess(app->process_handle, 0);
                ::CloseHandle(app->process_handle);
                app->process_handle = nullptr;
# else
                int child = (int)(uint64_t)app->process_handle;
                kill(child, SIGKILL);
                app->process_handle = nullptr;
# endif
                app->exited = true;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            utils::filesystem::remove_path(app->working_dir);
        }

        void daemon_s_service::check_apps()
        {
            std::vector< std::shared_ptr< app_internal> > apps, delete_list;

            _lock.lock_read();
            for (auto& pkg : _apps)
            {
                for (auto& app : pkg.second->apps)
                {
                    apps.push_back(app.second);
                }
            }
            _lock.unlock_read();
            
            for (auto& app : apps)
            {
                if (app->process_handle)
                {
# ifdef _WIN32
                    if (WAIT_OBJECT_0 == ::WaitForSingleObject(app->process_handle, 0))
                    {
                        app->exited = true;
                        DWORD exit_code = 0xdeadbeef;
                        ::GetExitCodeProcess(app->process_handle, &exit_code);
                        ::CloseHandle(app->process_handle);
                        app->process_handle = nullptr;

                        dinfo("app %s exits (code = %x), with working dir = %s, port = %d",
                            app->info.app_type.c_str(),
                            exit_code,
                            app->working_dir.c_str(),
                            (int)app->working_port
                        );
                    }
# else
                    int child = (int)(uint64_t)app->process_handle;
                    
                    // see if the process is not there
                    if (getpgid(child) < 0)
                    {
                        app->exited = true;
                        app->process_handle = nullptr;

                        dinfo("app %s exits, with working dir = %s, port = %d",
                            app->info.app_type.c_str(),
                            app->working_dir.c_str(),
                            (int)app->working_port
                            );
                    }
# endif
                }

                if (app->exited)
                {
                    delete_list.push_back(app);
                }
            }

            for (auto& app : delete_list)
            {
                auto cap_app = app;
                kill_app(std::move(cap_app));
                update_configuration_on_meta_server(config_type::CT_REMOVE, std::move(app));
            }
        }
        
        void daemon_s_service::update_configuration_on_meta_server(::dsn::replication::config_type::type type, std::shared_ptr<app_internal>&& app)
        {
            rpc_address node = primary_address();
            node.assign_ipv4(node.ip(), app->working_port);
            
            dsn_message_t msg = dsn_msg_create_request(RPC_CM_UPDATE_PARTITION_CONFIGURATION);

            std::shared_ptr<configuration_update_request> request(new configuration_update_request);
            request->info.is_stateful = false;
            request->config = app->configuration;
            request->type = type;
            request->node = node;
            request->host_node = primary_address();

            if (type == config_type::CT_REMOVE)
            {
                auto it = std::remove(
                    app->configuration.secondaries.begin(),
                    app->configuration.secondaries.end(),
                    request->host_node
                    );
                app->configuration.secondaries.erase(it);

                it = std::remove(
                    app->configuration.last_drops.begin(),
                    app->configuration.last_drops.end(),
                    node
                    );
                app->configuration.last_drops.erase(it);
            }
            else
            {
                app->configuration.secondaries.emplace_back(request->host_node);
                app->configuration.last_drops.emplace_back(node);
            }

            ::dsn::marshall(msg, *request);

            rpc::call(
                _fd->get_servers(),
                msg,
                this,
                [=, cap_app = std::move(app)](error_code err, dsn_message_t reqmsg, dsn_message_t response) mutable
                {
                    on_update_configuration_on_meta_server_reply(type, std::move(cap_app), err, reqmsg, response);
                }
                );
        }

        void daemon_s_service::on_update_configuration_on_meta_server_reply(
            ::dsn::replication::config_type::type type, std::shared_ptr<app_internal> &&  app,
            error_code err, dsn_message_t request, dsn_message_t response
            )
        {
            if (false == _online)
            {
                err.end_tracking();
                return;
            }

            configuration_update_response resp;
            if (err == ERR_OK)
            {
                ::dsn::unmarshall(response, resp);
                err = resp.err;
            }
            else if (err == ERR_TIMEOUT)
            {
                rpc::call(
                    _fd->get_servers(),
                    request,
                    this,
                    [=, cap_app = std::move(app)](error_code err, dsn_message_t reqmsg, dsn_message_t response) mutable
                    {
                        on_update_configuration_on_meta_server_reply(type, std::move(cap_app), err, reqmsg, response);
                    }
                    );
            }
            else
            {
                if (type == config_type::CT_ADD_SECONDARY)
                    kill_app(std::move(app));
            }
        }


        ::dsn::error_code daemon::start(int argc, char** argv)
        {
            _daemon_s_svc.reset(new daemon_s_service());
            _daemon_s_svc->open_service();
            return ::dsn::ERR_OK;
        }

        ::dsn::error_code daemon::stop(bool cleanup)
        {
            _daemon_s_svc->close_service();
            _daemon_s_svc = nullptr;
            return ERR_OK;
        }

        daemon::daemon(dsn_gpid gpid)
            : ::dsn::service_app(gpid)
        {

        }

        daemon::~daemon()
        {

        }
    }
}
