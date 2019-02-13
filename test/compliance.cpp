#include <thread>
#include <memory>

#include <Poco/AutoPtr.h>
#include <Poco/ConsoleChannel.h>
#include <Poco/Formatter.h>
#include <Poco/Logger.h>
#include <Poco/PatternFormatter.h>
#include <Poco/NullChannel.h>
#include <Poco/Process.h>
#include <Poco/Pipe.h>
#include <Poco/RegularExpression.h>

#include <gtest.h>

#include <sinsp.h>
#include <configuration.h>
#include <coclient.h>

using namespace std;
using namespace Poco;

struct task_defs_t {
	std::string schedule;
	uint64_t id;
	std::string name;
	std::string module;
	std::string scraper_id;
	std::string sleep_time;
	std::string rc;
	bool successful;
	std::string start_time;
	std::vector<std::string> future_runs;

	std::shared_ptr<Poco::RegularExpression> failure_details_re;

	task_defs_t() {};
	task_defs_t(std::string tschedule,
		    uint64_t tid,
		    std::string tname,
		    std::string tmodule,
		    std::string tscraper_id,
		    std::string tsleep_time)
		: schedule(tschedule),
		  id(tid),
		  name(tname),
		  module(tmodule),
		  scraper_id(tscraper_id),
		  sleep_time(tsleep_time),
		  rc("0"),
		  successful(true),
		  start_time("")
		{};

	task_defs_t(std::string tschedule,
		    uint64_t tid,
		    std::string tname,
		    std::string tmodule,
		    std::string tscraper_id,
		    std::string tsleep_time,
		    std::string trc,
		    bool tsuccessful,
		    std::string tfailure_details)
		: schedule(tschedule),
		  id(tid),
		  name(tname),
		  module(tmodule),
		  scraper_id(tscraper_id),
		  sleep_time(tsleep_time),
		  rc(trc),
		  successful(tsuccessful),
		  start_time("")
		{
			failure_details_re = make_shared<Poco::RegularExpression>(tfailure_details);
		};

	task_defs_t(std::string tschedule,
		    uint64_t tid,
		    std::string tname,
		    std::string tmodule,
		    std::string tscraper_id,
		    std::string tsleep_time,
		    std::string tstart_time,
		    std::vector<std::string> tfuture_runs)
		: schedule(tschedule),
		  id(tid),
		  name(tname),
		  module(tmodule),
		  scraper_id(tscraper_id),
		  sleep_time(tsleep_time),
		  rc("0"),
		  successful(true),
		  start_time(tstart_time),
		  future_runs(tfuture_runs)
		{};
};

class compliance_test : public testing::Test
{
protected:
	virtual void SetUp()
	{
		// The (global) logger only needs to be set up once
		if(!g_log)
		{
			AutoPtr<Formatter> formatter(new PatternFormatter("%Y-%m-%d %H:%M:%S.%i, %P, %p, %t"));

			AutoPtr<Channel> console_channel(new ConsoleChannel());
			AutoPtr<Channel> formatting_channel_console(new FormattingChannel(formatter, console_channel));
			// To enable debug logging, change the tailing -1 to Message::Priority::PRIO_DEBUG
			Logger &loggerc = Logger::create("DraiosLogC", formatting_channel_console, -1);

			AutoPtr<Channel> null_channel(new NullChannel());
			Logger &nullc = Logger::create("NullC", null_channel, -1);

			g_log = std::unique_ptr<dragent_logger>(new dragent_logger(&nullc, &loggerc, &nullc));
		}

		string cointerface_sock = "./resources/compliance_test.sock";

		Process::Args args{"-sock", cointerface_sock,
				"-use_json=false",
				"-modules_dir=./resources/modules_dir"
				};

		// Start a cointerface process to act as the
		// server. Capture its output and log everything at
		// debug level.
		m_colog = make_shared<Pipe>();
		m_cointerface = make_shared<ProcessHandle>(Process::launch("./resources/cointerface", args, NULL, m_colog.get(), NULL));

		thread log_reader = thread([] (shared_ptr<Pipe> colog) {
			PipeInputStream cologstr(*colog);
			string line;

			while (std::getline(cologstr, line))
			{
				g_log->information(line);
			}
		}, m_colog);

		log_reader.detach();

		// Wait for the process in a sub-thread so it
		// is reaped as soon as it exits. This is
		// necessary as Process::isRunning returns
		// true for zombie processes.
		thread waiter = thread([this] () {
			int status;
			waitpid(m_cointerface->id(), &status, 0);
		});

		waiter.detach();

		Thread::sleep(500);

		if (!Process::isRunning(*m_cointerface))
		{
			FAIL() << "cointerface process not running after 1 second";
		}

		m_grpc_conn = grpc_connect<sdc_internal::ComplianceModuleMgr::Stub>("unix:" + cointerface_sock);
		m_grpc_start = make_shared<streaming_grpc_client(&sdc_internal::ComplianceModuleMgr::Stub::AsyncStart)>(m_grpc_conn);
		m_grpc_load = make_shared<unary_grpc_client(&sdc_internal::ComplianceModuleMgr::Stub::AsyncLoad)>(m_grpc_conn);
		m_grpc_stop = make_shared<unary_grpc_client(&sdc_internal::ComplianceModuleMgr::Stub::AsyncStop)>(m_grpc_conn);
		m_grpc_get_future_runs = make_shared<unary_grpc_client(&sdc_internal::ComplianceModuleMgr::Stub::AsyncGetFutureRuns)>(m_grpc_conn);
		m_grpc_run_tasks = make_shared<unary_grpc_client(&sdc_internal::ComplianceModuleMgr::Stub::AsyncRunTasks)>(m_grpc_conn);

		// Also create a server listening on the statsd port
		if ((m_statsd_sock = socket(PF_INET, SOCK_DGRAM, 0)) < 0)
		{
			FAIL() << "Could not create socket for fake statsd server: " << strerror(errno);
		}

		struct sockaddr_in saddr;
		memset(&saddr, 0, sizeof(saddr));
		saddr.sin_family = AF_INET;
		saddr.sin_addr.s_addr = htonl(INADDR_ANY);
		saddr.sin_port = htons(8125);

		if(bind(m_statsd_sock, (struct sockaddr *) &saddr, sizeof(saddr)) < 0)
		{
			FAIL() << "Can't bind() to port for fake statsd server: " << strerror(errno);
		}

		// Set a default timeout of 100ms, so we can signal the thread

		struct timeval ts;
		ts.tv_sec = 0;
		ts.tv_usec = 100000;
		if (setsockopt(m_statsd_sock, SOL_SOCKET, SO_RCVTIMEO, &ts, sizeof(ts)) < 0)
		{
			FAIL() << "Can't set timeout of 5 seconds for fake statsd server: " << strerror(errno);
		}

		m_statsd_server_done = false;

		// In a thread, receive statsd metrics and update m_metrics
		m_statsd_server = thread([this] ()
                {
			while (!m_statsd_server_done)
			{
				char buf[1024];
				ssize_t recv_len;

				if((recv_len = recv(m_statsd_sock, buf, sizeof(buf), 0)) < 0)
				{
					if(errno != EAGAIN)
					{
						fprintf(stderr, "Could not receive statsd metric: %s\n", strerror(errno));
					}
				}
				else
				{
					std::lock_guard<std::mutex> lock(m_metrics_mutex);

					m_metrics.insert(string(buf, recv_len));
				}
			}
		});
	}

	virtual void TearDown()
	{
		if(m_cointerface)
		{
			Process::kill(*m_cointerface);
		}

		m_statsd_server_done = 1;
	        m_statsd_server.join();

		if(close(m_statsd_sock) < 0)
		{
			FAIL() << "Can't close statsd socket: " << strerror(errno);
		}

		m_grpc_load.reset();
		m_grpc_start.reset();
		m_grpc_stop.reset();
		m_grpc_run_tasks.reset();
		m_grpc_get_future_runs.reset();
		m_grpc_conn.reset();
		g_log->information("TearDown() complete");
	}

	void stop_tasks()
	{
		bool stopped = false;
		auto callback = [this, &stopped](bool successful, sdc_internal::comp_stop_result &res)
		{
			if(!successful)
			{
				FAIL() << "Stop() call was not successful";
			}

			if(!res.successful())
			{
				FAIL() << "Stop() call returned error " << res.errstr();
			}

			stopped = true;
		};

		sdc_internal::comp_stop stop;
		m_grpc_stop->do_rpc(stop, callback);

		// Wait up to 10 seconds
		for(uint32_t i=0; i < 1000; i++)
		{
			Poco::Thread::sleep(10);
			m_grpc_stop->process_queue();

			if(stopped)
			{
				return;
			}
		}

		FAIL() << "After 10 seconds, did not get response to Stop()";
	}

	void get_future_runs(task_defs_t &def)
	{
		bool received = false;
		sdc_internal::comp_future_runs future_runs;

		auto callback = [&](bool successful, sdc_internal::comp_future_runs &res)
		{
			if(!successful)
			{
				FAIL() << "GetFutureRuns() call was not successful";
			}

			if(!res.successful())
			{
				FAIL() << "GetFutureRuns() call returned error " << res.errstr();
			}

			g_log->debug(string("Return value from GetFutureRuns:") + res.DebugString());

			future_runs = res;
			received = true;
		};

		sdc_internal::comp_get_future_runs req;
		draiosproto::comp_task *task = req.mutable_task();

		task->set_id(def.id);
		task->set_name(def.name);
		task->set_mod_name(def.module);
		task->set_enabled(true);
		task->set_schedule(def.schedule);

		req.set_start(def.start_time);
		req.set_num_runs(5);

		m_grpc_get_future_runs->do_rpc(req, callback);

		// Wait up to 10 seconds
		for(uint32_t i=0; i < 1000; i++)
		{
			Poco::Thread::sleep(10);
			m_grpc_get_future_runs->process_queue();

			if(received)
			{
				ASSERT_EQ(uint32_t(future_runs.runs().size()), def.future_runs.size());

				for(int32_t i=0; i < future_runs.runs().size(); i++)
				{
					ASSERT_STREQ(future_runs.runs(i).c_str(), def.future_runs.at(i).c_str());
				}

				return;
			}
		}

		FAIL() << "After 10 seconds, did not get response to GetFutureRuns()";
	}

	void start_tasks(std::vector<task_defs_t> &task_defs)
	{
		auto callback = [this](streaming_grpc::Status status, sdc_internal::comp_task_event &cevent)
		{
			ASSERT_NE(status, streaming_grpc::ERROR);

			if(!status == streaming_grpc::OK)
			{
				return;
			}

			if(!cevent.call_successful())
			{
				m_errors[cevent.task_name()].push_back(cevent.errstr());
			}
			else
			{
				for(int i=0; i < cevent.events().events_size(); i++)
				{
					m_events[cevent.task_name()].push_back(cevent.events().events(i));
				}

				ASSERT_STREQ(cevent.results().machine_id().c_str(), "test-machine-id");
				ASSERT_STREQ(cevent.results().customer_id().c_str(), "test-customer-id");

				for(int i=0; i < cevent.results().results_size(); i++)
				{
					m_results[cevent.task_name()].push_back(cevent.results().results(i));
				}
			}
		};

		sdc_internal::comp_start start;

		for(auto &def: task_defs)
		{
			draiosproto::comp_task *task = start.mutable_calendar()->add_tasks();
			task->set_id(def.id);
			task->set_name(def.name);
			task->set_mod_name(def.module);
			task->set_enabled(true);
			task->set_schedule(def.schedule);

			draiosproto::comp_task_param *param = task->add_task_params();
			param->set_key("iter");
			param->set_val(def.scraper_id);

			param = task->add_task_params();
			param->set_key("sleepTime");
			param->set_val(def.sleep_time);

			param = task->add_task_params();
			param->set_key("rc");
			param->set_val(def.rc);
		}

		start.set_machine_id("test-machine-id");
		start.set_customer_id("test-customer-id");
		start.set_send_failed_results(true);

		m_grpc_start->do_rpc(start, callback);
        }

	void run_tasks(std::vector<task_defs_t> &task_defs)
	{
		bool received_response = false;

		auto callback =
			[&](bool successful, sdc_internal::comp_run_result &res)
				{
					ASSERT_TRUE(successful);

					ASSERT_TRUE(res.successful()) << string("Could not run compliance tasks (") + res.errstr();

					received_response = true;
				};

		draiosproto::comp_run run;

		for(auto &def: task_defs)
		{
			run.add_task_ids(def.id);
		}

		m_grpc_run_tasks->do_rpc(run, callback);

		// Wait up to 10 seconds
		for(uint32_t i=0; i < 1000; i++)
		{
			Poco::Thread::sleep(10);
			m_grpc_run_tasks->process_queue();

			if(received_response)
			{
				return;
			}
		}

		FAIL() << "After 10 seconds, did not get response to RunTasks()";
        }

	void verify_task_result(task_defs_t &def, uint64_t num_results=1)
	{
		// Wait up to 10 seconds
		for(uint32_t i=0; i < 1000; i++)
		{
			Poco::Thread::sleep(10);
			m_grpc_start->process_queue();

			if(m_results.find(def.name) != m_results.end() &&
			   m_results[def.name].size() >= num_results)
			{
				break;
			}
		}

		ASSERT_TRUE(m_results.find(def.name) != m_results.end()) << "After 10 seconds, did not see any results with expected values for task " << def.name;
		ASSERT_EQ(m_results[def.name].size(), num_results) << "After 10 seconds, did not see any results with expected values for task " << def.name;
		auto &result = m_results[def.name].front();

		ASSERT_EQ(result.successful(), def.successful);

		if(result.successful())
		{
			Json::Value ext_result;
			Json::Reader reader;
			ASSERT_TRUE(reader.parse(result.ext_result(), ext_result));

			ASSERT_EQ(ext_result["id"].asUInt64(), def.id);
			ASSERT_STREQ(ext_result["taskName"].asString().c_str(), def.name.c_str());
			ASSERT_EQ(ext_result["testsRun"].asUInt64(), strtoul(def.scraper_id.c_str(), NULL, 10));
			ASSERT_EQ(ext_result["passCount"].asUInt64(), strtoul(def.scraper_id.c_str(), NULL, 10));
			ASSERT_STREQ(ext_result["risk"].asString().c_str(), "low");
		}
		else
		{
			ASSERT_TRUE(def.failure_details_re->match(result.failure_details()));
		}
	}

	void verify_task_event(task_defs_t &def)
	{
		// Wait up to 10 seconds
		for(uint32_t i=0; i < 1000; i++)
		{
			Poco::Thread::sleep(10);
			m_grpc_start->process_queue();

			if(m_results.find(def.name) != m_results.end())
			{
				break;
			}
		}

		ASSERT_TRUE(m_events.find(def.name) != m_events.end()) << "After 10 seconds, did not see any events with expected values for task " << def.name;
		ASSERT_EQ(m_events[def.name].size(), 1U) << "After 10 seconds, did not see any events with expected values for task " << def.name;
		auto &event = m_events[def.name].front();

		std::string output = "test output (task=" + def.name + " iter=" + def.scraper_id + ")";
		std::string output_json = "{\"task\":\"" + def.name + "\", \"iter\": " + def.scraper_id + "}";

		ASSERT_STREQ(event.task_name().c_str(), def.name.c_str());
		ASSERT_STREQ(event.container_id().c_str(), "test-container");
		ASSERT_STREQ(event.output().c_str(), output.c_str());
		ASSERT_STREQ(event.output_fields().at("task").c_str(), def.name.c_str());
		ASSERT_STREQ(event.output_fields().at("iter").c_str(), def.scraper_id.c_str());
	}

	void clear_results_events()
	{
		m_results.clear();
		m_events.clear();
		m_metrics.clear();
		m_errors.clear();
	}

	void verify_metric(task_defs_t &def)
	{
		std::string expected = string("compliance.") + def.name + ":tests_pass:" + def.scraper_id + "|g\n";

		// Wait up to 10 seconds
		for(uint32_t i=0; i < 1000; i++)
		{
			Poco::Thread::sleep(10);
			{
				std::lock_guard<std::mutex> lock(m_metrics_mutex);
				if(m_metrics.find(expected) != m_metrics.end())
				{
					return;
				}
			}
		}

		FAIL() << "After 10 seconds, did not see expected metric for task " << def.name;
	}

	void verify_error(std::string &task_name, std::string &expected)
	{
		// Wait up to 10 seconds
		for(uint32_t i=0; i < 1000; i++)
		{
			Poco::Thread::sleep(10);
			m_grpc_start->process_queue();

			if (m_errors.find(task_name) != m_errors.end())
			{
				for(auto &errstr : m_errors[task_name])
				{
					if (errstr == expected)
					{
						return;
					}
				}
			}
		}

		FAIL() << "After 10 seconds, did not see expected error \"" << expected << "\" for task name " << task_name;
	}

	shared_ptr<Pipe> m_colog;
	shared_ptr<ProcessHandle> m_cointerface;

	std::shared_ptr<sdc_internal::ComplianceModuleMgr::Stub> m_grpc_conn;
	std::shared_ptr<streaming_grpc_client(&sdc_internal::ComplianceModuleMgr::Stub::AsyncStart)> m_grpc_start;
	std::shared_ptr<unary_grpc_client(&sdc_internal::ComplianceModuleMgr::Stub::AsyncLoad)> m_grpc_load;
	std::shared_ptr<unary_grpc_client(&sdc_internal::ComplianceModuleMgr::Stub::AsyncStop)> m_grpc_stop;
	std::shared_ptr<unary_grpc_client(&sdc_internal::ComplianceModuleMgr::Stub::AsyncRunTasks)> m_grpc_run_tasks;
	std::shared_ptr<unary_grpc_client(&sdc_internal::ComplianceModuleMgr::Stub::AsyncGetFutureRuns)> m_grpc_get_future_runs;

	// Maps from task name to all the results that have been received for that task
	std::map<std::string, std::vector<draiosproto::comp_result>> m_results;
	std::map<std::string, std::vector<draiosproto::comp_event>> m_events;
	std::map<std::string, std::vector<std::string>> m_errors;

	// All the unique metrics that have ever been received by the fake statsd server
	std::set<std::string> m_metrics;
	std::mutex m_metrics_mutex;

	std::thread m_statsd_server;
	int m_statsd_sock;
	atomic<bool> m_statsd_server_done;
};

static std::vector<task_defs_t> one_task = {{"PT1H", 1, "my-task-1", "test-module", "1", "0"}};
static std::vector<task_defs_t> frequent_task = {{"PT10S", 1, "my-task-1", "test-module", "1", "0"}};
static std::vector<task_defs_t> task_slow = {{"PT1H", 1, "my-task-1", "test-module", "1", "5"}};
static std::vector<task_defs_t> one_task_alt_output = {{"PT1H", 1, "my-task-1", "test-module", "2", "0"}};
static std::vector<task_defs_t> task_two = {{"PT1H", 2, "my-task-2", "test-module", "2", "0"}};
static std::vector<task_defs_t> two_tasks = {{"PT1H", 1, "my-task-1", "test-module", "1", "0"}, {"PT1H", 2, "my-task-2", "test-module", "2", "0"}};
static std::vector<task_defs_t> two_tasks_alt_output = {{"PT1H", 1, "my-task-1", "test-module", "3", "0"}, {"PT1H", 2, "my-task-2", "test-module", "4", "0"}};
static std::vector<task_defs_t> one_task_twice = {{"R2/PT1S", 1, "my-task-1", "test-module", "1", "5"}};
static std::vector<task_defs_t> bad_schedule = {{"not-a-real-schedule", 1, "bad-schedule-task", "test-module", "1", "5"}};
static std::vector<task_defs_t> bad_schedule_2 = {{"PT1K1M", 1, "bad-schedule-task-2", "test-module", "1", "5"}};
static std::vector<task_defs_t> bad_schedule_leading_junk = {{"junkPT1H", 1, "bad-schedule-task-leading-junk", "test-module", "1", "5"}};
static std::vector<task_defs_t> bad_schedule_trailing_junk = {{"PT-1H", 1, "bad-schedule-task-trailing-junk", "test-module", "1", "5"}};
static std::vector<task_defs_t> bad_module = {{"PT1H", 1, "bad-module-task", "not-a-real-module", "1", "0"}};
static std::vector<task_defs_t> exit_failure = {{"PT1H", 1, "exit-failure-task-1", "test-module", "1", "0", "1", false, "^module test-module via {Path=.*test/resources/modules_dir/test-module/run.sh Args=\\[.*/test/resources/modules_dir/test-module/run.sh 0 1\\] Env=\\[.*\\] Dir=.*/test/resources/modules_dir/test-module} exited with error \\(exit status 1\\) Stdout: \"This is to stdout\\n\" Stderr: \"This is to stderr\\n\""}};

// This module is defined, but its command line doesn't exist, meaning it will fail every time it is run.
static std::vector<task_defs_t> fail_module = {{"PT1H", 1, "fail-task-1", "fail-module", "1", "0", "1", false, "^Could not start module fail-module via {Path=.*/test/resources/modules_dir/fail-module/not-runnable Args=\\[.*/test/resources/modules_dir/fail-module/not-runnable 0 1\\] Env=\\[.*\\] Dir=.*/test/resources/modules_dir/fail-module} \\(fork/exec .*/test/resources/modules_dir/fail-module/not-runnable: permission denied\\)"}};

static std::vector<task_defs_t> multiple_intervals = {{"[R1/PT1S, PT1H]", 1, "multiple-intervals", "test-module", "1", "0"}};

static std::vector<task_defs_t> multiple_intervals_2 = {{"[R1/PT1S, R1/PT2S]", 1, "multiple-intervals-2", "test-module", "1", "0"}};

// The current time will be added to the interval
static std::vector<task_defs_t> explicit_start_time = {{"/P1D", 1, "my-task-1", "test-module", "1", "0"}};

static std::vector<task_defs_t> future_runs_twice_daily = {{"06:00:00Z/PT12H", 1, "next-run-1", "test-module", "1", "0", "2018-11-14T00:00:00Z", {"2018-11-14T06:00:00Z", "2018-11-14T18:00:00Z", "2018-11-15T06:00:00Z", "2018-11-15T18:00:00Z", "2018-11-16T06:00:00Z"}}};
static std::vector<task_defs_t> future_runs_once_daily_6am = {{"06:00:00Z/P1D", 1, "next-run-1", "test-module", "1", "0", "2018-11-14T00:00:00Z", {"2018-11-14T06:00:00Z", "2018-11-15T06:00:00Z", "2018-11-16T06:00:00Z", "2018-11-17T06:00:00Z", "2018-11-18T06:00:00Z"}}};
static std::vector<task_defs_t> future_runs_once_daily_6pm = {{"18:00:00Z/P1D", 1, "next-run-1", "test-module", "1", "0", "2018-11-14T00:00:00Z", {"2018-11-14T18:00:00Z", "2018-11-15T18:00:00Z", "2018-11-16T18:00:00Z", "2018-11-17T18:00:00Z", "2018-11-18T18:00:00Z"}}};
static std::vector<task_defs_t> future_runs_weekly_monday_6am = {{"2018-11-12T06:00:00Z/P1W", 1, "next-run-1", "test-module", "1", "0", "2018-11-14T00:00:00Z", {"2018-11-19T06:00:00Z", "2018-11-26T06:00:00Z", "2018-12-03T06:00:00Z", "2018-12-10T06:00:00Z", "2018-12-17T06:00:00Z"}}};
static std::vector<task_defs_t> future_runs_weekly_monday_6pm = {{"2018-11-12T18:00:00Z/P1W", 1, "next-run-1", "test-module", "1", "0", "2018-11-14T00:00:00Z", {"2018-11-19T18:00:00Z", "2018-11-26T18:00:00Z", "2018-12-03T18:00:00Z", "2018-12-10T18:00:00Z", "2018-12-17T18:00:00Z"}}};
static std::vector<task_defs_t> future_runs_weekly_wednesday_6am = {{"2018-11-14T06:00:00Z/P1W", 1, "next-run-1", "test-module", "1", "0", "2018-11-14T00:00:00Z", {"2018-11-14T06:00:00Z", "2018-11-21T06:00:00Z", "2018-11-28T06:00:00Z", "2018-12-05T06:00:00Z", "2018-12-12T06:00:00Z"}}};
static std::vector<task_defs_t> future_runs_weekly_wednesday_6pm = {{"2018-11-14T18:00:00Z/P1W", 1, "next-run-1", "test-module", "1", "0", "2018-11-14T00:00:00Z", {"2018-11-14T18:00:00Z", "2018-11-21T18:00:00Z", "2018-11-28T18:00:00Z", "2018-12-05T18:00:00Z", "2018-12-12T18:00:00Z"}}};
static std::vector<task_defs_t> future_runs_weekly_friday_6am = {{"2018-11-16T06:00:00Z/P1W", 1, "next-run-1", "test-module", "1", "0", "2018-11-14T00:00:00Z", {"2018-11-16T06:00:00Z", "2018-11-23T06:00:00Z", "2018-11-30T06:00:00Z", "2018-12-07T06:00:00Z", "2018-12-14T06:00:00Z"}}};
static std::vector<task_defs_t> future_runs_weekly_friday_6pm = {{"2018-11-16T18:00:00Z/P1W", 1, "next-run-1", "test-module", "1", "0", "2018-11-14T00:00:00Z", {"2018-11-16T18:00:00Z", "2018-11-23T18:00:00Z", "2018-11-30T18:00:00Z", "2018-12-07T18:00:00Z", "2018-12-14T18:00:00Z"}}};
static std::vector<task_defs_t> future_runs_twice_monthly_6am = {{"[2018-11-01T06:00:00Z/P1M, 2018-11-14T06:00:00Z/P1M]", 1, "next-run-1", "test-module", "1", "0", "2018-11-14T00:00:00Z", {"2018-11-14T06:00:00Z", "2018-12-01T06:00:00Z", "2018-12-14T06:00:00Z", "2019-01-01T06:00:00Z", "2019-01-14T06:00:00Z"}}};
static std::vector<task_defs_t> future_runs_twice_monthly_6pm = {{"[2018-11-01T18:00:00Z/P1M, 2018-11-14T18:00:00Z/P1M]", 1, "next-run-1", "test-module", "1", "0", "2018-11-14T00:00:00Z", {"2018-11-14T18:00:00Z", "2018-12-01T18:00:00Z", "2018-12-14T18:00:00Z", "2019-01-01T18:00:00Z", "2019-01-14T18:00:00Z"}}};
static std::vector<task_defs_t> future_runs_once_monthly_1st_6am = {{"2018-11-01T06:00:00Z/P1M", 1, "next-run-1", "test-module", "1", "0", "2018-11-14T00:00:00Z", {"2018-12-01T06:00:00Z", "2019-01-01T06:00:00Z", "2019-02-01T06:00:00Z", "2019-03-01T06:00:00Z", "2019-04-01T06:00:00Z"}}};
static std::vector<task_defs_t> future_runs_once_monthly_1st_6pm = {{"2018-11-01T18:00:00Z/P1M", 1, "next-run-1", "test-module", "1", "0", "2018-11-14T00:00:00Z", {"2018-12-01T18:00:00Z", "2019-01-01T18:00:00Z", "2019-02-01T18:00:00Z", "2019-03-01T18:00:00Z", "2019-04-01T18:00:00Z"}}};
static std::vector<task_defs_t> future_runs_once_monthly_14th_6am = {{"2018-11-14T06:00:00Z/P1M", 1, "next-run-1", "test-module", "1", "0", "2018-11-14T00:00:00Z", {"2018-11-14T06:00:00Z", "2018-12-14T06:00:00Z", "2019-01-14T06:00:00Z", "2019-02-14T06:00:00Z", "2019-03-14T06:00:00Z"}}};
static std::vector<task_defs_t> future_runs_once_monthly_14th_6pm = {{"2018-11-14T18:00:00Z/P1M", 1, "next-run-1", "test-module", "1", "0", "2018-11-14T00:00:00Z", {"2018-11-14T18:00:00Z", "2018-12-14T18:00:00Z", "2019-01-14T18:00:00Z", "2019-02-14T18:00:00Z", "2019-03-14T18:00:00Z"}}};


// Test cases:
//   - DONE A single task with multiple intervals
//   - A task with an explicit start time + interval
//   - Add some endpoint/method that returns a list of the next 10 or so times each task will run, and use that in tests.

TEST_F(compliance_test, load)
{
	bool got_response = false;

	auto callback = [this, &got_response](bool successful, sdc_internal::comp_load_result &lresult)
	{
		got_response = true;
		ASSERT_TRUE(successful);

		ASSERT_EQ(lresult.statuses_size(), 4);

		for(auto &status : lresult.statuses())
		{
			if (strcmp(status.mod_name().c_str(), "docker-bench-security") != 0 &&
			    strcmp(status.mod_name().c_str(), "kube-bench") != 0 &&
			    strcmp(status.mod_name().c_str(), "test-module") != 0 &&
			    strcmp(status.mod_name().c_str(), "fail-module") != 0)
			{
				FAIL() << "Unexpected module found: " << status.mod_name();
			}

			ASSERT_TRUE(status.running());
			ASSERT_EQ(status.has_errstr(), false);
		}
	};

	sdc_internal::comp_load load;

	load.set_machine_id("test-machine-id");
	load.set_customer_id("test-customer-id");

	m_grpc_load->do_rpc(load, callback);

	// Wait up to 10 seconds
	for(uint32_t i=0; i < 1000 && !got_response; i++)
	{
		Poco::Thread::sleep(10);
		m_grpc_load->process_queue();
	}

	ASSERT_TRUE(got_response) << "10 seconds after Load(), did not receive any response";
}

TEST_F(compliance_test, start)
{
	start_tasks(one_task);
	verify_task_result(one_task[0]);
	verify_task_event(one_task[0]);
	verify_metric(one_task[0]);

	stop_tasks();
}

TEST_F(compliance_test, start_frequent)
{
	start_tasks(frequent_task);
	verify_task_result(frequent_task[0]);
	verify_task_event(frequent_task[0]);
	verify_metric(frequent_task[0]);

	stop_tasks();
}

TEST_F(compliance_test, multiple_start)
{
	start_tasks(one_task);
	verify_task_result(one_task[0]);
	verify_task_event(one_task[0]);
	verify_metric(one_task[0]);
	clear_results_events();

	start_tasks(one_task_alt_output);
	verify_task_result(one_task_alt_output[0]);
	verify_task_event(one_task_alt_output[0]);
	verify_metric(one_task_alt_output[0]);

	stop_tasks();
}

TEST_F(compliance_test, start_after_stop)
{
	start_tasks(one_task);
	verify_task_result(one_task[0]);
	verify_task_event(one_task[0]);
	verify_metric(one_task[0]);
	stop_tasks();
	clear_results_events();

	start_tasks(one_task_alt_output);
	verify_task_result(one_task_alt_output[0]);
	verify_task_event(one_task_alt_output[0]);
	verify_metric(one_task_alt_output[0]);

	stop_tasks();
}

TEST_F(compliance_test, multiple_tasks_same_module)
{
	start_tasks(two_tasks);
	verify_task_result(two_tasks[0]);
	verify_task_event(two_tasks[0]);
	verify_metric(two_tasks[0]);

	verify_task_result(two_tasks[1]);
	verify_task_event(two_tasks[1]);
	verify_metric(two_tasks[1]);

	stop_tasks();
}

TEST_F(compliance_test, multiple_tasks_multiple_start)
{
	start_tasks(two_tasks);
	verify_task_result(two_tasks[0]);
	verify_task_event(two_tasks[0]);
	verify_metric(two_tasks[0]);

	verify_task_result(two_tasks[1]);
	verify_task_event(two_tasks[1]);
	verify_metric(two_tasks[1]);

	clear_results_events();

	start_tasks(two_tasks_alt_output);
	verify_task_result(two_tasks_alt_output[0]);
	verify_task_event(two_tasks_alt_output[0]);
	verify_metric(two_tasks_alt_output[0]);

	verify_task_result(two_tasks_alt_output[1]);
	verify_task_event(two_tasks_alt_output[1]);
	verify_metric(two_tasks_alt_output[1]);

	stop_tasks();
}

TEST_F(compliance_test, start_cancels)
{
	start_tasks(task_slow);

	sleep(1);

	start_tasks(task_two);

	verify_task_result(task_two[0]);
	verify_task_event(task_two[0]);
	verify_metric(task_two[0]);

	sleep(10);
	ASSERT_TRUE(m_results.find(task_slow[0].name) == m_results.end());
	ASSERT_TRUE(m_events.find(task_slow[0].name) == m_events.end());
	ASSERT_TRUE(m_metrics.find(task_slow[0].name) == m_metrics.end());

	stop_tasks();
}

TEST_F(compliance_test, overlapping_tasks)
{
	start_tasks(one_task_twice);

	verify_task_result(one_task_twice[0]);
	verify_task_event(one_task_twice[0]);
	verify_metric(one_task_twice[0]);

	// Ensure that there is only a single result/event. The first
	// task runs for 5 seconds, so the second invocation
	// should have been skipped.

	sleep(10);

	ASSERT_EQ(m_events[one_task_twice[0].name].size(), 1U);
	ASSERT_EQ(m_results[one_task_twice[0].name].size(), 1U);

	stop_tasks();
}

TEST_F(compliance_test, bad_schedule)
{
	std::string expected = "Could not schedule task bad-schedule-task: Could not parse duration from schedule not-a-real-schedule: did not match expected pattern";

	start_tasks(bad_schedule);

	verify_error(bad_schedule[0].name, expected);

	stop_tasks();
}

TEST_F(compliance_test, bad_schedule_2)
{
	std::string expected = "Could not schedule task bad-schedule-task-2: Could not parse duration from schedule PT1K1M: did not match expected pattern";

	start_tasks(bad_schedule_2);

	verify_error(bad_schedule_2[0].name, expected);

	stop_tasks();
}

TEST_F(compliance_test, bad_schedule_leading_junk)
{
	std::string expected = "Could not schedule task bad-schedule-task-leading-junk: Could not parse duration from schedule junkPT1H: did not match expected pattern";

	start_tasks(bad_schedule_leading_junk);

	verify_error(bad_schedule_leading_junk[0].name, expected);

	stop_tasks();
}

TEST_F(compliance_test, bad_schedule_trailing_junk)
{
	std::string expected = "Could not schedule task bad-schedule-task-trailing-junk: Could not parse duration from schedule PT-1H: did not match expected pattern";

	start_tasks(bad_schedule_trailing_junk);

	verify_error(bad_schedule_trailing_junk[0].name, expected);

	stop_tasks();
}

TEST_F(compliance_test, bad_module)
{
	start_tasks(bad_module);

	std::string expected = "Could not schedule task bad-module-task: Module not-a-real-module does not exist";

	verify_error(bad_module[0].name, expected);

	stop_tasks();
}

TEST_F(compliance_test, exit_failure)
{
	start_tasks(exit_failure);

	verify_task_result(exit_failure[0]);

	stop_tasks();
}

TEST_F(compliance_test, fail_module)
{
	start_tasks(fail_module);

	verify_task_result(fail_module[0]);

	stop_tasks();
}

TEST_F(compliance_test, multiple_intervals)
{
	start_tasks(multiple_intervals);

	// Should be 1 result from the "run now" task, and one for the first interval.
	verify_task_result(multiple_intervals[0], 2);

	stop_tasks();
}


TEST_F(compliance_test, multiple_intervals_2)
{
	start_tasks(multiple_intervals_2);

	// Should be 1 result from the "run now" task, and one for each interval
	verify_task_result(multiple_intervals_2[0], 3);

	stop_tasks();
}

TEST_F(compliance_test, run_tasks)
{
	start_tasks(one_task);

	verify_task_result(one_task[0]);
	verify_task_event(one_task[0]);
	verify_metric(one_task[0]);

	clear_results_events();

	run_tasks(one_task);

	// Normally this would fail other than the fact that we
	// triggered running the task out-of-band.
	verify_task_result(one_task[0]);
	verify_task_event(one_task[0]);
	verify_metric(one_task[0]);

	stop_tasks();
}

TEST_F(compliance_test, explicit_start_time)
{
	char timestr[32];
	time_t now;

	time(&now);

	now += 10;

	strftime(timestr, sizeof(timestr), "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));

	explicit_start_time[0].schedule = string(timestr) + explicit_start_time[0].schedule;

	start_tasks(explicit_start_time);

	// Start a thread to read results
	bool done = false;
	std::thread result_reader = thread([&] ()
        {
		while(!done)
		{
			Poco::Thread::sleep(10);
			m_grpc_start->process_queue();
		}
	});

	sleep(5);

	// There should be only a single result so far, which reflects
	// the initial "run now" task.
	ASSERT_EQ(m_events[explicit_start_time[0].name].size(), 1U);
	ASSERT_EQ(m_results[explicit_start_time[0].name].size(), 1U);

	sleep(10);

	// Now there should be 2 results, as the start time for the schedule has occurred
	ASSERT_EQ(m_events[explicit_start_time[0].name].size(), 2U);
	ASSERT_EQ(m_results[explicit_start_time[0].name].size(), 2U);

	done = true;
	result_reader.join();

	stop_tasks();
}

TEST_F(compliance_test, future_runs_twice_daily)
{
	get_future_runs(future_runs_twice_daily[0]);
}

TEST_F(compliance_test, future_runs_once_daily_6am)
{
	get_future_runs(future_runs_once_daily_6am[0]);
}

TEST_F(compliance_test, future_runs_once_daily_6pm)
{
	get_future_runs(future_runs_once_daily_6pm[0]);
}

TEST_F(compliance_test, future_runs_weekly_monday_6am)
{
	get_future_runs(future_runs_weekly_monday_6am[0]);
}

TEST_F(compliance_test, future_runs_weekly_monday_6pm)
{
	get_future_runs(future_runs_weekly_monday_6pm[0]);
}

TEST_F(compliance_test, future_runs_weekly_wednesday_6am)
{
	get_future_runs(future_runs_weekly_wednesday_6am[0]);
}

TEST_F(compliance_test, future_runs_weekly_wednesday_6pm)
{
	get_future_runs(future_runs_weekly_wednesday_6pm[0]);
}

TEST_F(compliance_test, future_runs_weekly_friday_6am)
{
	get_future_runs(future_runs_weekly_friday_6am[0]);
}

TEST_F(compliance_test, future_runs_weekly_friday_6pm)
{
	get_future_runs(future_runs_weekly_friday_6pm[0]);
}

TEST_F(compliance_test, future_runs_twice_monthly_6am)
{
	get_future_runs(future_runs_twice_monthly_6am[0]);
}

TEST_F(compliance_test, future_runs_twice_monthly_6pm)
{
	get_future_runs(future_runs_twice_monthly_6pm[0]);
}

TEST_F(compliance_test, future_runs_once_monthly_1st_6am)
{
	get_future_runs(future_runs_once_monthly_1st_6am[0]);
}

TEST_F(compliance_test, future_runs_once_monthly_1st_6pm)
{
	get_future_runs(future_runs_once_monthly_1st_6pm[0]);
}

TEST_F(compliance_test, future_runs_once_monthly_14th_6am)
{
	get_future_runs(future_runs_once_monthly_14th_6am[0]);
}

TEST_F(compliance_test, future_runs_once_monthly_14th_6pm)
{
	get_future_runs(future_runs_once_monthly_14th_6pm[0]);
}

