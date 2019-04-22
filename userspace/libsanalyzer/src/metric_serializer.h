/**
 * @file
 *
 * Interface to metric_serializer -- an abstract base class for analyzer
 * metric serialization.
 *
 * @copyright Copyright (c) 2019 Sysdig Inc., All Rights Reserved
 */
#pragma once

#include "internal_metrics.h"
#include <atomic>
#include <memory>
#include <mutex>

namespace draiosproto { class metrics; }

class analyzer_callback_interface;
class sinsp_configuration;

namespace libsanalyzer
{

/**
 * Abstract base class for analyzer metric serialization.
 */
class metric_serializer
{
public:
	/**
	 * Sentinel event number that indicates that a serialization operation
	 * was not triggered by an event.
	 */
	const static uint64_t NO_EVENT_NUMBER;

	/**
	 * Enable clients of the serialize() API to pass in data in the form
	 * in which we'll store it.  Client code should not use this class
	 * outside of calls to serialize().
	 */
	class data
	{
	public:
		data(uint64_t evt_num,
		     uint64_t ts,
		     uint32_t sampling_ratio,
		     double prev_flush_cpu_pct,
		     uint64_t prev_flushes_duration_ns,
		     std::atomic<bool>& metrics_sent,
		     double my_cpuload,
		     bool extra_internal_metrics,
		     const draiosproto::metrics& metrics);

		const uint64_t m_evt_num;
		const uint64_t m_ts;
		const uint32_t m_sampling_ratio;
		const double m_prev_flush_cpu_pct;
		const uint64_t m_prev_flushes_duration_ns;
		std::atomic<bool>& m_metrics_sent;
		const double m_my_cpuload;
		const bool m_extra_internal_metrics;
		draiosproto::metrics m_metrics;
	};


	/**
	 * Initialize this metric_serializer.
	 *
	 * @param[in] internal_metrics The internal metrics to serialize.
	 * @param[in] configuration    The current state of the
	 *                             sinsp configuration.
	 */
	metric_serializer(const internal_metrics::sptr_t& internal_metrics,
			  const sinsp_configuration* configuration);

	virtual ~metric_serializer() = default;

	/**
	 * Start the serialization process for the given data.  This process
	 * may be performed asynchronously, client code must handle async
	 * updates to anything passed by reference to data's constructor.
	 *
	 * @param[in] data The data to serialize.
	 */
	virtual void serialize(std::unique_ptr<data>&& data) = 0;

	/**
	 * Wait for any potentially async serialization operations to complete.
	 */
	virtual void drain() const = 0;

	/**
	 * Update the internal metrics to the given value.
	 *
	 * @param[in] im The new internal metrics.
	 */
	void set_internal_metrics(internal_metrics::sptr_t im);

	/** Returns a smart pointer to the current internal metrics. */
	const internal_metrics::sptr_t& get_internal_metrics() const;

	/**
	 * Update the sample callback handler to th given cb.
	 *
	 * @param[in] cb The new callback handler.
	 */
	void set_sample_callback(analyzer_callback_interface* cb);

	/** Returns a pointer to the current sample callback. */
	analyzer_callback_interface* get_sample_callback() const;

	/**
	 * Returns true if this metric_serializer is configured to emit
	 * metrics to file, false otherwise.
	 */
	bool get_emit_metrics_to_file() const;

	/**
	 * Returns true if this metric_serializer is configured to compress
	 * metrics that are written to file, false otherwise.  This method's
	 * return value is meaningful only when get_emit_metrics_to_file()
	 * returns true.
	 */
	bool get_compress_metrics() const;

	/**
	 * Returns the path to the directory into which this metric_serializer
	 * will write metrics to file.  This method's return value is
	 * meaningful only when get_emit_metrics_to_file() returns true.
	 */
	const std::string& get_metrics_directory() const;

	/**
	 * Updates the configuration state of this metric_serializer.
	 *
	 * @param[in] configuration A pointer to the new configuration.
	 */
	void update_configuration(const sinsp_configuration* configuration);

private:
	mutable std::mutex m_mutex;
	internal_metrics::sptr_t m_internal_metrics;
	const sinsp_configuration* m_configuration;
	analyzer_callback_interface* m_sample_callback;
};

} // end namespace libsanalyzer
