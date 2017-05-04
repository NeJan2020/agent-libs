#pragma once

// A security policy represents a step in the security event
// workflow. It contains a scope and a set of actions to perform if
// the policy triggers.
//
// This class is virtual void and is the base class for falco_policy.

#include <memory>
#include <list>

#include <google/protobuf/text_format.h>

#include <draios.pb.h>
#include "coclient.h"

#include <falco_engine.h>
#include <falco_events.h>

class security_mgr;

class SINSP_PUBLIC security_policy
{
public:
	security_policy(security_mgr *mgr,
			dragent_configuration *confguration,
			uint64_t id,
			const std::string &name,
			const google::protobuf::RepeatedPtrField<draiosproto::action> &actions,
			std::shared_ptr<coclient> &coclient,
			bool enabled);
	virtual ~security_policy();

	// Try to match the sinsp event against this policy. If the
	// policy matches, returns a policy_event message with details
	// on the event. Returns NULL otherwise.
	virtual draiosproto::policy_event *process_event(sinsp_evt *evt) = 0;

	// Perform the actions for this policy, using the information
	// from the given event. Any action results will be added to
	// event.
	//
	// The policy then owns event and is responsible for deleting it.
	//
	// Returns true if policy processing should stop without
	// continuing on to later policies.
	bool perform_actions(sinsp_evt *evt, draiosproto::policy_event *event);

	// Check the list of outstanding actions and see if any are
	// complete. If they are, pass the policy event to the security mgr.
	void check_outstanding_actions(uint64_t ts_ns);

	// Return a string representation of this rule.
	virtual std::string &to_string();

	// Return the name of this policy.
	std::string &name();

protected:
	// Keeps track of any policy events and their outstanding
	// actions. When all actions are complete, the policy will
	// send the policy event message.
	class actions_state
	{
	public:
		actions_state(draiosproto::policy_event *event,
			      uint32_t num_remaining_actions)
			: m_event(event),
  			  m_num_remaining_actions(num_remaining_actions),
			  m_send_now(false)
		{
		};

		virtual ~actions_state()
		{
		}

		shared_ptr<draiosproto::policy_event> m_event;
		uint32_t m_num_remaining_actions;

		// If true, this policy event must be sent as soon as
		// all actions are complete.
		bool m_send_now;
	};

	std::list<actions_state> m_outstanding_actions;

	google::protobuf::TextFormat::Printer m_print;

	security_mgr *m_mgr;
	dragent_configuration *m_configuration;
	uint64_t m_id;
	std::string m_name;
	list<draiosproto::action> m_actions;
	bool m_enabled;
	std::shared_ptr<coclient> m_coclient;
	std::string m_str;
};

class SINSP_PUBLIC falco_security_policy : public security_policy
{
public:
	falco_security_policy(security_mgr *mgr,
			      dragent_configuration *configuration,
			      const draiosproto::policy &policy,
			      sinsp *inspector,
			      shared_ptr<falco_engine> &falco_engine,
			      shared_ptr<falco_events> &falco_events,
			      std::shared_ptr<coclient> &coclient);

	virtual ~falco_security_policy();

	draiosproto::policy_event *process_event(sinsp_evt *evt);

	// Return a string representation of this rule.
	virtual std::string &to_string();

private:

	std::string m_rule_filter;
	std::set<std::string> m_tags;
	shared_ptr<falco_engine> m_falco_engine;
	shared_ptr<falco_events> m_falco_events;
	sinsp_evt_formatter_cache m_formatters;
	std::string m_fstr;

	uint16_t m_ruleset_id;
};
