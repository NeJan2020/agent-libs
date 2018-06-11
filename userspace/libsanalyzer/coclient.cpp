// From sysdig, for g_logger
#include "sinsp.h"
#include "sinsp_int.h"
#include "logger.h"

#include "analyzer_utils.h"
#include "coclient.h"

using namespace std;

#ifndef CYGWING_AGENT

std::string coclient::default_domain_sock = string("/run/cointerface.sock"); // install prefix will be prepended
uint32_t coclient::m_max_loop_evts = 100;

coclient::coclient(const std::string& install_prefix):
	m_domain_sock(install_prefix + default_domain_sock),
	m_outstanding_swarm_state(false)
{
	m_print.SetSingleLineMode(true);
}

coclient::~coclient()
{
}

void coclient::connect()
{
	// XXX - Using 1 channel per stub causes problems reconnecting if
	// cointerface crashes.  Temporarily hack a shared channel with a
	// function static, but clean it up in the pending coclient changes
	static std::shared_ptr<grpc::Channel> shared_chan =
		grpc::CreateChannel(string("unix:") + m_domain_sock,
				    grpc::InsecureChannelCredentials());
	m_stub = sdc_internal::CoInterface::NewStub(shared_chan);
}

void coclient::prepare(google::protobuf::Message *request_msg,
		       sdc_internal::cointerface_message_type msg_type,
		       response_cb_t response_cb)
{
	if(!m_stub)
	{
		connect();
	}

	string tmp;
	m_print.PrintToString(*request_msg, &tmp);
	g_logger.log("Sending message to cointerface: " + tmp, sinsp_logger::SEV_DEBUG);

	call_context *call = new call_context();

	call->msg_type = msg_type;
	call->response_cb = response_cb;

	// Perform the (async) rpc
	// This only works because we only have a single rpc function
	// for a given request message type.
	switch(msg_type) {
		sdc_internal::ping *ping;
		sdc_internal::docker_command *docker_command;
		sdc_internal::swarm_state_command *sscmd;
		sdc_internal::orchestrator_events_stream_command *orchestrator_events_stream_command;

	case sdc_internal::PING:
		// Start the rpc call and have the pong reader read the response when
		// it's ready.
		ping = static_cast<sdc_internal::ping *>(request_msg);
		call->pong_reader = m_stub->AsyncPerformPing(&call->ctx, *ping, &m_cq);

		// Tell the pong reader to write the response into the
		// response message, update status with whether or not the
		// rpc could be performed, and tag the rpc with a tag
		// that is the address of the call struct.
		call->response_msg = make_unique<sdc_internal::pong>();
		call->pong_reader->Finish(static_cast<sdc_internal::pong *>(call->response_msg.get()), &call->status, (void*)call);
		break;

	case sdc_internal::SWARM_STATE_COMMAND:
		sscmd = static_cast<sdc_internal::swarm_state_command *>(request_msg);
		call->swarm_state_reader = m_stub->AsyncPerformSwarmState(&call->ctx, *sscmd, &m_cq);

		call->response_msg = make_unique<sdc_internal::swarm_state_result>();
		call->swarm_state_reader->Finish(static_cast<sdc_internal::swarm_state_result *>(call->response_msg.get()), &call->status, (void*)call);
		break;

	case sdc_internal::DOCKER_COMMAND:
                // Start the rpc call and have the docker_cmd_result reader read the response when
                // it's ready.
		docker_command = static_cast<sdc_internal::docker_command *>(request_msg);
		call->docker_cmd_result_reader = m_stub->AsyncPerformDockerCommand(&call->ctx, *docker_command, &m_cq);

		// Tell the reader to write the response into the
		// response message, update status with whether or not
		// the rpc could be performed, and tag the rpc with a
		// tag that is the address of the call struct.
		call->response_msg = make_unique<sdc_internal::docker_command_result>();
		call->docker_cmd_result_reader->Finish(static_cast<sdc_internal::docker_command_result *>(call->response_msg.get()),
						       &call->status, (void*)call);

		break;
	case sdc_internal::ORCHESTRATOR_EVENTS_STREAM_COMMAND:
		call->is_streaming = true;

		orchestrator_events_stream_command = static_cast<sdc_internal::orchestrator_events_stream_command *>(request_msg);
		call->orchestrator_events_reader = m_stub->AsyncPerformOrchestratorEventsStream(&call->ctx, *orchestrator_events_stream_command, &m_cq, (void *)call);

		call->response_msg = make_unique<draiosproto::congroup_update_event>();

		break;
	default:
		g_logger.log("Unknown message type " + to_string(msg_type), sinsp_logger::SEV_ERROR);
		break;
	}
}

bool coclient::process_queue()
{
	uint32_t count = 0;
	bool okay = true;
	while (okay && count < m_max_loop_evts)
	{
		okay = next();
		count++;
	}
	return okay;
}

bool coclient::next()
{
	void *tag;
	bool updates_ok;
	grpc::CompletionQueue::NextStatus status;

	status = m_cq.AsyncNext(&tag, &updates_ok, gpr_time_0(GPR_CLOCK_REALTIME));

	if(status == grpc::CompletionQueue::SHUTDOWN)
	{
		g_logger.log("cointerface process shut down, disconnecting", sinsp_logger::SEV_ERROR);
		m_stub = NULL;
		m_outstanding_swarm_state = false;
		return false;
	}
	else if(status == grpc::CompletionQueue::TIMEOUT)
	{
		return false;
	}

	call_context *call = static_cast<call_context *>(tag);

	if(call->msg_type == sdc_internal::SWARM_STATE_COMMAND)
	{
		m_outstanding_swarm_state = false;
	}

	if(!updates_ok) {
		m_stub = NULL;
		if(call->is_streaming) {
			glogf(sinsp_logger::SEV_WARNING,
			      "cointerface streaming RPC (%s) returned error", 
			      sdc_internal::cointerface_message_type_Name(call->msg_type).c_str());
			call->response_cb(false, nullptr);
		} else {
			glogf(sinsp_logger::SEV_ERROR,
			      "cointerface RPC (%s) could not be scheduled successfully",
			      sdc_internal::cointerface_message_type_Name(call->msg_type).c_str());
			delete call;
		}
		return true;
	}


	if (call->is_streaming) {
		//
		// Server-streaming RPC errors are detected by
		// updates_ok, so we can now assume that the
		// call was successful
		//
		call->status = grpc::Status::OK;
		switch(call->msg_type) {
		case(sdc_internal::ORCHESTRATOR_EVENTS_STREAM_COMMAND):
			call->orchestrator_events_reader->Read(static_cast<draiosproto::congroup_update_event *>(call->response_msg.get()), (void *)call);
			break;
		default:
			g_logger.log("Unknown streaming message type " + to_string(call->msg_type) + ", can't read response", sinsp_logger::SEV_ERROR);
			break;
		}
		//
		// The first response notify us that the server
		// is ready to send messages. If it's the case,
		// there's nothing else to do
		//
		if(!call->is_server_ready) {
			call->is_server_ready = true;
			g_logger.log("RPC streaming server connected and ready to send messages.", sinsp_logger::SEV_DEBUG);
			return true;
		}
	}

	if(call->status.ok()) {
		string tmp;
		m_print.PrintToString(*(call->response_msg), &tmp);

		g_logger.log("Got response from cointerface: " + tmp, sinsp_logger::SEV_DEBUG);

	} else {
		g_logger.log("cointerface rpc failed", sinsp_logger::SEV_DEBUG);
	}

	call->response_cb(call->status.ok(), call->response_msg.get());

	if (!call->is_streaming)
	{
		delete call;
	}

	return true;
}

void coclient::set_domain_sock(std::string &domain_sock)
{
	m_domain_sock = domain_sock;
}

void coclient::cleanup()
{
	Poco::File f(default_domain_sock);
	if(f.exists())
	{
		f.remove();
	}
}

void coclient::ping(int64_t token, response_cb_t response_cb)
{
	sdc_internal::ping ping;

	ping.set_token(token);

	prepare(&ping, sdc_internal::PING, response_cb);
}

void coclient::perform_docker_cmd(sdc_internal::docker_cmd_type cmd,
				  const string &container_id, response_cb_t response_cb)
{
	sdc_internal::docker_command docker_cmd;

	docker_cmd.set_cmd(cmd);
	docker_cmd.set_container_id(container_id);

	prepare(&docker_cmd, sdc_internal::DOCKER_COMMAND, response_cb);
}

void coclient::get_swarm_state(response_cb_t response_cb)
{
	if(m_outstanding_swarm_state)
	{
		g_logger.log("Swarm State requested while still pending", sinsp_logger::SEV_WARNING);
		return;
	}
	m_outstanding_swarm_state = true;

	sdc_internal::swarm_state_command cmd;
	prepare(&cmd, sdc_internal::SWARM_STATE_COMMAND, response_cb);
}

void coclient::get_orchestrator_events(sdc_internal::orchestrator_events_stream_command cmd,
										response_cb_t response_cb)
{
	prepare(&cmd, sdc_internal::ORCHESTRATOR_EVENTS_STREAM_COMMAND, response_cb);
}
#endif // CYGWING_AGENT
