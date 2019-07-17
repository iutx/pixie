#pragma once

#ifndef __linux__

#include "src/stirling/source_connector.h"

namespace pl {
namespace stirling {

DUMMY_SOURCE_CONNECTOR(SocketTraceConnector);

}  // namespace stirling
}  // namespace pl

#else

#include <bcc/BPF.h>

#include <deque>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "demos/applications/hipster_shop/reflection.h"
#include "src/common/grpcutils/service_descriptor_database.h"
#include "src/stirling/connection_tracker.h"
#include "src/stirling/socket_trace.h"
#include "src/stirling/source_connector.h"

DECLARE_string(http_response_header_filters);
DECLARE_bool(enable_parsing_protobufs);

OBJ_STRVIEW(http_trace_bcc_script, _binary_bcc_bpf_socket_trace_c_preprocessed);

namespace pl {
namespace stirling {

enum class HTTPContentType {
  kUnknown = 0,
  kJSON = 1,
  // We use gRPC instead of PB to be consistent with the wording used in gRPC.
  kGRPC = 2,
};

template <class TMessageType>
struct TraceRecord {
  const ConnectionTracker* tracker;
  TMessageType req_message;
  TMessageType resp_message;
};

class SocketTraceConnector : public SourceConnector {
 public:
  inline static const std::string_view kBCCScript = http_trace_bcc_script;

  // clang-format off
  static constexpr DataElement kHTTPElements[] = {
          {"time_", types::DataType::TIME64NS, types::PatternType::METRIC_COUNTER},
          {"pid", types::DataType::INT64, types::PatternType::GENERAL},
          // TODO(oazizi): Merge with pid, and use INT128, when available.
          {"pid_start_time", types::DataType::INT64, types::PatternType::GENERAL},
          // TODO(yzhao): Remove 'fd'.
          {"fd", types::DataType::INT64, types::PatternType::GENERAL},
          // TODO(yzhao): Remove 'event_type'. Now each record includes both req and resp.
          // 'event_type', which was added one each record includes only req or resp, is no longer
          // meaningful.
          {"event_type", types::DataType::STRING, types::PatternType::GENERAL_ENUM},
          // TODO(PL-519): Eventually, use uint128 to represent IP addresses, as will be resolved in
          // the Jira issue.
          {"remote_addr", types::DataType::STRING, types::PatternType::GENERAL},
          {"remote_port", types::DataType::INT64, types::PatternType::GENERAL},
          {"http_major_version", types::DataType::INT64, types::PatternType::GENERAL_ENUM},
          {"http_minor_version", types::DataType::INT64, types::PatternType::GENERAL_ENUM},
          // TODO(yzhao): Replace http_headers with req_headers and resp_headers. As both req and
          // resp have headers. req_headers are particularly more meaningful for gRPC/HTTP2.
          {"http_headers", types::DataType::STRING, types::PatternType::STRUCTURED},
          // Possible types are from HTTPContentType enum.
          {"http_content_type", types::DataType::INT64, types::PatternType::GENERAL_ENUM},
          {"http_req_method", types::DataType::STRING, types::PatternType::GENERAL_ENUM},
          {"http_req_path", types::DataType::STRING, types::PatternType::STRUCTURED},
          // TODO(yzhao): Add req_body for the payload of gRPC request.
          {"http_resp_status", types::DataType::INT64, types::PatternType::GENERAL_ENUM},
          {"http_resp_message", types::DataType::STRING, types::PatternType::STRUCTURED},
          {"http_resp_body", types::DataType::STRING, types::PatternType::STRUCTURED},
          {"http_resp_latency_ns", types::DataType::INT64, types::PatternType::METRIC_GAUGE}
  };
  // clang-format on

  static constexpr ConstStrView kHTTPPerfBufferNames[] = {
      "socket_open_conns",
      "socket_http_events",
      "socket_close_conns",
  };

  // Used in ReadPerfBuffer to drain the relevant perf buffers.
  static constexpr auto kHTTPPerfBuffers = ConstVectorView<ConstStrView>(kHTTPPerfBufferNames);

  static constexpr auto kHTTPTable = DataTableSchema("http_events", kHTTPElements);

  // clang-format off
  static constexpr DataElement kMySQLElements[] = {
          {"time_", types::DataType::TIME64NS, types::PatternType::METRIC_COUNTER},
          {"pid", types::DataType::INT64, types::PatternType::GENERAL},
          {"pid_start_time", types::DataType::INT64, types::PatternType::GENERAL},
          {"fd", types::DataType::INT64, types::PatternType::GENERAL},
          {"bpf_event", types::DataType::INT64, types::PatternType::GENERAL_ENUM},
          {"remote_addr", types::DataType::STRING, types::PatternType::GENERAL},
          {"remote_port", types::DataType::INT64, types::PatternType::GENERAL},
          {"body", types::DataType::STRING, types::PatternType::STRUCTURED},
  };
  // clang-format on

  static constexpr ConstStrView kMySQLPerfBufferNames[] = {
      "socket_open_conns",
      "socket_mysql_events",
      "socket_close_conns",
  };

  static constexpr auto kMySQLPerfBuffers = ConstVectorView<ConstStrView>(kMySQLPerfBufferNames);

  static constexpr auto kMySQLTable = DataTableSchema("mysql_events", kMySQLElements);

  static constexpr DataTableSchema kTablesArray[] = {kHTTPTable, kMySQLTable};
  static constexpr auto kTables = ConstVectorView<DataTableSchema>(kTablesArray);
  static constexpr uint32_t kHTTPTableNum = SourceConnector::TableNum(kTables, kHTTPTable);
  static constexpr uint32_t kMySQLTableNum = SourceConnector::TableNum(kTables, kMySQLTable);

  static constexpr std::chrono::milliseconds kDefaultSamplingPeriod{100};
  static constexpr std::chrono::milliseconds kDefaultPushPeriod{1000};

  // Dim 0: DataTables; dim 1: perfBuffer Names
  static constexpr ConstVectorView<ConstStrView> perfBufferNames[] = {kHTTPPerfBuffers,
                                                                      kMySQLPerfBuffers};
  static constexpr auto kTablePerfBufferMap =
      ConstVectorView<ConstVectorView<ConstStrView> >(perfBufferNames);

  static std::unique_ptr<SourceConnector> Create(std::string_view name) {
    return std::unique_ptr<SourceConnector>(new SocketTraceConnector(name));
  }

  Status InitImpl() override;
  Status StopImpl() override;
  void TransferDataImpl(uint32_t table_num, types::ColumnWrapperRecordBatch* record_batch) override;

  Status Configure(uint32_t protocol, uint64_t config_mask);

  /**
   * @brief Number of active ConnectionTrackers.
   *
   * Note: Multiple ConnectionTrackers on same TGID+FD are counted as 1.
   */
  size_t NumActiveConnections() const { return connection_trackers_.size(); }

  /**
   * @brief Gets a pointer to a ConnectionTracker by conn_id.
   *
   * @param connid The connection to get.
   * @return Pointer to the ConnectionTracker, or nullptr if it does not exist.
   */
  const ConnectionTracker* GetConnectionTracker(struct conn_id_t connid) const;

  void TestOnlyConfigure(uint32_t protocol, uint64_t config_mask) {
    config_mask_[protocol] = config_mask;
  }
  static void TestOnlySetHTTPResponseHeaderFilter(HTTPHeaderFilter filter) {
    http_response_header_filter_ = std::move(filter);
  }

  // This function causes the perf buffer to be read, and triggers callbacks per message.
  // TODO(oazizi): This function is only public for testing purposes. Make private?
  void ReadPerfBuffer(uint32_t table_num);

 private:
  // ReadPerfBuffer poll callback functions (must be static).
  // These are used by the static variables below, and have to be placed here.
  static void HandleHTTPProbeOutput(void* cb_cookie, void* data, int data_size);
  static void HandleMySQLProbeOutput(void* cb_cookie, void* data, int data_size);
  static void HandleProbeLoss(void* cb_cookie, uint64_t lost);
  static void HandleCloseProbeOutput(void* cb_cookie, void* data, int data_size);
  static void HandleOpenProbeOutput(void* cb_cookie, void* data, int data_size);

  // Describes a kprobe that should be attached with the BPF::attach_kprobe().
  struct ProbeSpec {
    std::string kernel_fn_short_name;
    std::string trace_fn_name;
    bpf_probe_attach_type attach_type;
  };

  struct PerfBufferSpec {
    // Name is same as the perf buffer inside bcc_bpf/socket_trace.c.
    std::string name;
    perf_reader_raw_cb probe_output_fn;
    perf_reader_lost_cb probe_loss_fn;
    uint32_t num_pages;
  };

  static inline const std::vector<ProbeSpec> kProbeSpecs = {
      {"connect", "syscall__probe_entry_connect", bpf_probe_attach_type::BPF_PROBE_ENTRY},
      {"connect", "syscall__probe_ret_connect", bpf_probe_attach_type::BPF_PROBE_RETURN},
      {"accept", "syscall__probe_entry_accept", bpf_probe_attach_type::BPF_PROBE_ENTRY},
      {"accept", "syscall__probe_ret_accept", bpf_probe_attach_type::BPF_PROBE_RETURN},
      {"accept4", "syscall__probe_entry_accept4", bpf_probe_attach_type::BPF_PROBE_ENTRY},
      {"accept4", "syscall__probe_ret_accept4", bpf_probe_attach_type::BPF_PROBE_RETURN},
      {"write", "syscall__probe_entry_write", bpf_probe_attach_type::BPF_PROBE_ENTRY},
      {"write", "syscall__probe_ret_write", bpf_probe_attach_type::BPF_PROBE_RETURN},
      {"send", "syscall__probe_entry_send", bpf_probe_attach_type::BPF_PROBE_ENTRY},
      {"send", "syscall__probe_ret_send", bpf_probe_attach_type::BPF_PROBE_RETURN},
      {"sendto", "syscall__probe_entry_sendto", bpf_probe_attach_type::BPF_PROBE_ENTRY},
      {"sendto", "syscall__probe_ret_sendto", bpf_probe_attach_type::BPF_PROBE_RETURN},
      {"read", "syscall__probe_entry_read", bpf_probe_attach_type::BPF_PROBE_ENTRY},
      {"read", "syscall__probe_ret_read", bpf_probe_attach_type::BPF_PROBE_RETURN},
      {"recv", "syscall__probe_entry_recv", bpf_probe_attach_type::BPF_PROBE_ENTRY},
      {"recv", "syscall__probe_ret_recv", bpf_probe_attach_type::BPF_PROBE_RETURN},
      {"recvfrom", "syscall__probe_entry_recv", bpf_probe_attach_type::BPF_PROBE_ENTRY},
      {"recvfrom", "syscall__probe_ret_recv", bpf_probe_attach_type::BPF_PROBE_RETURN},
      {"close", "syscall__probe_close", bpf_probe_attach_type::BPF_PROBE_ENTRY},
  };

  // TODO(oazizi): Remove send and recv probes once we are confident that they don't trace anything.
  //               Note that send/recv are not in the syscall table
  //               (https://filippo.io/linux-syscall-table/), but are defined as SYSCALL_DEFINE4 in
  //               https://elixir.bootlin.com/linux/latest/source/net/socket.c.

  static constexpr uint32_t kDefaultPageCount = 8;
  static inline const std::vector<PerfBufferSpec> kPerfBufferSpecs = {
      // For data events. The order must be consistent with output tables.
      {"socket_http_events", &SocketTraceConnector::HandleHTTPProbeOutput,
       &SocketTraceConnector::HandleProbeLoss, kDefaultPageCount},
      {"socket_mysql_events", &SocketTraceConnector::HandleMySQLProbeOutput,
       &SocketTraceConnector::HandleProbeLoss, kDefaultPageCount},

      // For non-data events. Must not mix with the above perf buffers for data events.
      {"socket_open_conns", &SocketTraceConnector::HandleOpenProbeOutput,
       &SocketTraceConnector::HandleProbeLoss, kDefaultPageCount},
      {"socket_close_conns", &SocketTraceConnector::HandleCloseProbeOutput,
       &SocketTraceConnector::HandleProbeLoss, kDefaultPageCount},
  };

  inline static HTTPHeaderFilter http_response_header_filter_;
  // TODO(yzhao): We will remove this once finalized the mechanism of lazy protobuf parse.
  inline static grpc::ServiceDescriptorDatabase grpc_desc_db_{
      demos::hipster_shop::GetFileDescriptorSet()};

  explicit SocketTraceConnector(std::string_view source_name)
      : SourceConnector(source_name, kTables, kDefaultSamplingPeriod, kDefaultPushPeriod) {
    // TODO(yzhao): Is there a better place/time to grab the flags?
    http_response_header_filter_ = ParseHTTPHeaderFilters(FLAGS_http_response_header_filters);
    config_mask_.resize(kNumProtocols);
  }

  // Events from BPF.
  // TODO(oazizi/yzhao): These all operate based on pass-by-value, which copies.
  //                     The Handle* functions should call make_unique() of new corresponding
  //                     objects, and these functions should take unique_ptrs.
  void AcceptDataEvent(std::unique_ptr<SocketDataEvent> event);
  void AcceptOpenConnEvent(conn_info_t conn_info);
  void AcceptCloseConnEvent(conn_info_t conn_info);

  // Transfers the data from stream buffers (from AcceptDataEvent()) to record_batch.
  void TransferStreamData(uint32_t table_num, types::ColumnWrapperRecordBatch* record_batch);

  // Transfer of an HTTP Response Event to the HTTP Response Table in the table store.
  template <class TMessageType>
  void TransferStreams(TrafficProtocol protocol, types::ColumnWrapperRecordBatch* record_batch);

  template <class TMessageType>
  void ProcessMessages(const ConnectionTracker& conn_tracker,
                       std::deque<TMessageType>* req_messages,
                       std::deque<TMessageType>* resp_messages,
                       types::ColumnWrapperRecordBatch* record_batch);

  template <class TMessageType>
  static void ConsumeMessage(TraceRecord<TMessageType> record,
                             types::ColumnWrapperRecordBatch* record_batch);

  template <class TMessageType>
  static bool SelectMessage(const TraceRecord<TMessageType>& record);

  template <class TMessageType>
  static void AppendMessage(TraceRecord<TMessageType> record,
                            types::ColumnWrapperRecordBatch* record_batch);

  // Transfer of a MySQL Event to the MySQL Table.
  // TODO(oazizi/yzhao): Change to use std::unique_ptr.
  void TransferMySQLEvent(SocketDataEvent event, types::ColumnWrapperRecordBatch* record_batch);

  ebpf::BPF bpf_;

  // Note that the inner map cannot be a vector, because there is no guaranteed order
  // in which events are read from perf buffers.
  // Inner map could be a priority_queue, but benchmarks showed better performance with a std::map.
  // Key is {PID, FD} for outer map (see GetStreamId()), and generation for inner map.
  std::unordered_map<uint64_t, std::map<uint64_t, ConnectionTracker> > connection_trackers_;

  // For MySQL tracing only. Will go away when MySQL uses streams.
  types::ColumnWrapperRecordBatch* record_batch_ = nullptr;

  std::vector<uint64_t> config_mask_;

  FRIEND_TEST(SocketTraceConnectorTest, AppendNonContiguousEvents);
  FRIEND_TEST(SocketTraceConnectorTest, NoEvents);
  FRIEND_TEST(SocketTraceConnectorTest, End2end);
  FRIEND_TEST(SocketTraceConnectorTest, RequestResponseMatching);
  FRIEND_TEST(SocketTraceConnectorTest, ConnectionCleanupInOrder);
  FRIEND_TEST(SocketTraceConnectorTest, ConnectionCleanupOutOfOrder);
  FRIEND_TEST(SocketTraceConnectorTest, ConnectionCleanupMissingDataEvent);
  FRIEND_TEST(SocketTraceConnectorTest, ConnectionCleanupOldGenerations);
  FRIEND_TEST(SocketTraceConnectorTest, ConnectionCleanupInactiveDead);
  FRIEND_TEST(SocketTraceConnectorTest, ConnectionCleanupInactiveAlive);
};

}  // namespace stirling
}  // namespace pl

#endif
