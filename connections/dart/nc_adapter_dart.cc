// Copyright 2024 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "connections/dart/nc_adapter_dart.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "absl/strings/string_view.h"
#include "third_party/dart_lang/v2/runtime/include/dart_api.h"
#include "third_party/dart_lang/v2/runtime/include/dart_api_dl.h"
#include "third_party/dart_lang/v2/runtime/include/dart_native_api.h"
#include "connections/c/nc.h"
#include "connections/c/nc_types.h"
#include "internal/platform/byte_array.h"
#include "internal/platform/count_down_latch.h"
#include "internal/platform/logging.h"
#include "internal/platform/prng.h"

static nearby::CountDownLatch *adapter_finished;

static Dart_Port port;
static DiscoveryListenerDart current_discovery_listener_dart;
static ConnectionListenerDart current_connection_listener_dart;
static PayloadListenerDart current_payload_listener_dart;

NC_STRATEGY_TYPE GetStrategy(StrategyDart strategy) {
  switch (strategy) {
    case STRATEGY_P2P_CLUSTER:
      return NC_STRATEGY_TYPE_P2P_CLUSTER;
    case STRATEGY_P2P_POINT_TO_POINT:
      return NC_STRATEGY_TYPE_P2P_POINT_TO_POINT;
    case STRATEGY_P2P_STAR:
      return NC_STRATEGY_TYPE_P2P_STAR;
    default:
      break;
  }
  return NC_STRATEGY_TYPE_NONE;
}

nearby::ByteArray ConvertBluetoothMacAddress(absl::string_view address) {
  return nearby::ByteArray(std::string(address));
}

NC_PAYLOAD_ID GeneratePayloadId() { return nearby::Prng().NextInt64(); }

void ResultCB(NC_STATUS status) {
  NEARBY_LOG(INFO, "ResultCB is called.");
  (void)status;  // Avoid unused parameter warning
  Dart_CObject dart_object_result_callback;
  dart_object_result_callback.type = Dart_CObject_kInt64;
  dart_object_result_callback.value.as_int64 = static_cast<int64_t>(status);
  const bool result = Dart_PostCObject_DL(port, &dart_object_result_callback);
  if (!result) {
    NEARBY_LOG(INFO, "Posting message to port failed.");
  }
  adapter_finished->CountDown();
}

void ListenerInitiatedCB(
    const char *endpoint_id,
    const NC_CONNECTION_RESPONSE_INFO &connection_response_info) {
  NEARBY_LOG(INFO, "Advertising initiated: id=%s", endpoint_id);

  Dart_CObject dart_object_endpoint_id;
  dart_object_endpoint_id.type = Dart_CObject_kString;
  dart_object_endpoint_id.value.as_string = const_cast<char *>(endpoint_id);

  Dart_CObject dart_object_endpoint_info;
  dart_object_endpoint_info.type = Dart_CObject_kString;
  dart_object_endpoint_info.value.as_string = const_cast<char *>(
      std::string(connection_response_info.remote_endpoint_info.data,
                  connection_response_info.remote_endpoint_info.size)
          .c_str());

  Dart_CObject *elements[2];
  elements[0] = &dart_object_endpoint_id;
  elements[1] = &dart_object_endpoint_info;

  Dart_CObject dart_object_initiated;
  dart_object_initiated.type = Dart_CObject_kArray;
  dart_object_initiated.value.as_array.length = 2;
  dart_object_initiated.value.as_array.values = elements;

  const bool result =
      Dart_PostCObject_DL(current_connection_listener_dart.initiated_dart_port,
                          &dart_object_initiated);
  if (!result) {
    NEARBY_LOG(INFO, "Posting message to port failed.");
  }
}

void ListenerAcceptedCB(const char *endpoint_id) {
  NEARBY_LOG(INFO, "Advertising accepted: id=%s", endpoint_id);
  Dart_CObject dart_object_accepted;
  dart_object_accepted.type = Dart_CObject_kString;
  dart_object_accepted.value.as_string = const_cast<char *>(endpoint_id);
  const bool result =
      Dart_PostCObject_DL(current_connection_listener_dart.accepted_dart_port,
                          &dart_object_accepted);
  if (!result) {
    NEARBY_LOG(INFO, "Posting message to port failed.");
  }
}

void ListenerRejectedCB(const char *endpoint_id, NC_STATUS status) {
  NEARBY_LOG(INFO, "Advertising rejected: id=%s", endpoint_id);
  Dart_CObject dart_object_rejected;
  dart_object_rejected.type = Dart_CObject_kString;
  dart_object_rejected.value.as_string = const_cast<char *>(endpoint_id);
  const bool result =
      Dart_PostCObject_DL(current_connection_listener_dart.rejected_dart_port,
                          &dart_object_rejected);
  if (!result) {
    NEARBY_LOG(INFO, "Posting message to port failed.");
  }
}

void ListenerDisconnectedCB(const char *endpoint_id) {
  NEARBY_LOG(INFO, "Advertising disconnected: id=%s", endpoint_id);
  Dart_CObject dart_object_disconnected;
  dart_object_disconnected.type = Dart_CObject_kString;
  dart_object_disconnected.value.as_string = const_cast<char *>(endpoint_id);
  const bool result = Dart_PostCObject_DL(
      current_connection_listener_dart.disconnected_dart_port,
      &dart_object_disconnected);
  if (!result) {
    NEARBY_LOG(INFO, "Posting message to port failed.");
  }
}

void ListenerBandwidthChangedCB(const char *endpoint_id, NC_MEDIUM medium) {
  NEARBY_LOG(INFO, "Advertising bandwidth changed: id=%s", endpoint_id);
  Dart_CObject dart_object_bandwidth_changed;

  dart_object_bandwidth_changed.type = Dart_CObject_kString;
  dart_object_bandwidth_changed.value.as_string =
      const_cast<char *>(endpoint_id);
  const bool result = Dart_PostCObject_DL(
      current_connection_listener_dart.bandwidth_changed_dart_port,
      &dart_object_bandwidth_changed);
  if (!result) {
    NEARBY_LOG(INFO, "Posting message to port failed.");
  }
}

void ListenerEndpointFoundCB(const char *endpoint_id,
                             const NC_DATA &endpoint_info,
                             const char *service_id) {
  NEARBY_LOG(INFO, "Device discovered: id=%s", endpoint_id);
  NEARBY_LOG(INFO, "Device discovered: service_id=%s", service_id);
  NEARBY_LOG(INFO, "Device discovered: info=%s", endpoint_info);

  Dart_CObject dart_object_endpoint_id;
  dart_object_endpoint_id.type = Dart_CObject_kString;
  dart_object_endpoint_id.value.as_string = const_cast<char *>(endpoint_id);

  Dart_CObject dart_object_endpoint_info;
  dart_object_endpoint_info.type = Dart_CObject_kString;
  dart_object_endpoint_info.value.as_string = const_cast<char *>(
      std::string(endpoint_info.data, endpoint_info.size).c_str());

  Dart_CObject *elements[2];
  elements[0] = &dart_object_endpoint_id;
  elements[1] = &dart_object_endpoint_info;

  Dart_CObject dart_object_found;
  dart_object_found.type = Dart_CObject_kArray;
  dart_object_found.value.as_array.length = 2;
  dart_object_found.value.as_array.values = elements;
  const bool result = Dart_PostCObject_DL(
      current_discovery_listener_dart.found_dart_port, &dart_object_found);
  if (!result) {
    NEARBY_LOG(INFO, "Posting message to port failed.");
  }
}

void ListenerEndpointLostCB(const char *endpoint_id) {
  NEARBY_LOG(INFO, "Device lost: id=%s", endpoint_id);
  Dart_CObject dart_object_lost;
  dart_object_lost.type = Dart_CObject_kString;
  dart_object_lost.value.as_string = const_cast<char *>(endpoint_id);
  const bool result = Dart_PostCObject_DL(
      current_discovery_listener_dart.lost_dart_port, &dart_object_lost);
  if (!result) {
    NEARBY_LOG(INFO, "Posting message to port failed.");
  }
}

void ListenerEndpointDistanceChangedCB(const char *endpoint_id,
                                       NC_DISTANCE_INFO distance_info) {
  (void)distance_info;  // Avoid unused parameter warning
  NEARBY_LOG(INFO, "Device distance changed: id=%s", endpoint_id);
  Dart_CObject dart_object_distance_changed;
  dart_object_distance_changed.type = Dart_CObject_kString;
  dart_object_distance_changed.value.as_string =
      const_cast<char *>(endpoint_id);
  const bool result = Dart_PostCObject_DL(
      current_discovery_listener_dart.distance_changed_dart_port,
      &dart_object_distance_changed);
  if (!result) {
    NEARBY_LOG(INFO, "Posting message to port failed.");
  }
}

void ListenerPayloadCB(const char *endpoint_id, const NC_PAYLOAD &payload) {
  NEARBY_LOG(INFO,
             "Payload callback called. id: %s, "
             "payload_id: %d, type: %d",
             endpoint_id, payload.id, payload.type);

  Dart_CObject dart_object_endpoint_id;
  dart_object_endpoint_id.type = Dart_CObject_kString;
  dart_object_endpoint_id.value.as_string = const_cast<char *>(endpoint_id);

  Dart_CObject dart_object_payload_id;
  dart_object_payload_id.type = Dart_CObject_kInt64;
  dart_object_payload_id.value.as_int64 = payload.id;

  switch (payload.type) {
    case NC_PAYLOAD_TYPE_BYTES: {
      const char *bytes = payload.content.bytes.content.data;
      size_t bytes_size = payload.content.bytes.content.size;

      if (bytes_size == 0) {
        NEARBY_LOG(INFO, "Failed to get the payload as bytes.");
        return;
      }

      Dart_CObject dart_object_bytes;
      dart_object_bytes.type = Dart_CObject_kTypedData;
      dart_object_bytes.value.as_typed_data = {
          .type = Dart_TypedData_kUint8,
          .length = static_cast<intptr_t>(bytes_size),
          .values = reinterpret_cast<const uint8_t *>(bytes),
      };

      Dart_CObject *elements[] = {
          &dart_object_endpoint_id,
          &dart_object_payload_id,
          &dart_object_bytes,
      };

      Dart_CObject dart_object_payload;
      dart_object_payload.type = Dart_CObject_kArray;
      dart_object_payload.value.as_array.length = 3;
      dart_object_payload.value.as_array.values = elements;
      if (!Dart_PostCObject_DL(
              current_payload_listener_dart.initial_byte_info_port,
              &dart_object_payload)) {
        NEARBY_LOG(INFO, "Posting message to port failed.");
      }
      return;
    }
    case NC_PAYLOAD_TYPE_STREAM: {
      Dart_CObject *elements[] = {
          &dart_object_endpoint_id,
          &dart_object_payload_id,
      };

      Dart_CObject dart_object_payload;
      dart_object_payload.type = Dart_CObject_kArray;
      dart_object_payload.value.as_array.length = 2;
      dart_object_payload.value.as_array.values = elements;
      if (!Dart_PostCObject_DL(
              current_payload_listener_dart.initial_stream_info_port,
              &dart_object_payload)) {
        NEARBY_LOG(INFO, "Posting message to port failed.");
      }
      return;
    }
    case NC_PAYLOAD_TYPE_FILE: {
      Dart_CObject dart_object_offset;
      dart_object_offset.type = Dart_CObject_kInt64;
      dart_object_offset.value.as_int64 = payload.content.file.offset;

      std::string path = payload.content.file.file_name;
      Dart_CObject dart_object_path;
      dart_object_path.type = Dart_CObject_kString;
      dart_object_path.value.as_string = const_cast<char *>(path.c_str());

      Dart_CObject *elements[] = {
          &dart_object_endpoint_id,
          &dart_object_payload_id,
          &dart_object_offset,
          &dart_object_path,
      };

      Dart_CObject dart_object_payload;
      dart_object_payload.type = Dart_CObject_kArray;
      dart_object_payload.value.as_array.length = 4;
      dart_object_payload.value.as_array.values = elements;
      if (!Dart_PostCObject_DL(
              current_payload_listener_dart.initial_file_info_port,
              &dart_object_payload)) {
        NEARBY_LOG(INFO, "Posting message to port failed.");
      }
      return;
    }
    default:
      NEARBY_LOG(INFO, "Invalid payload type.");
      return;
  }
}

void ListenerPayloadProgressCB(
    const char *endpoint_id,
    const NC_PAYLOAD_PROGRESS_INFO &payload_progress_info) {
  NEARBY_LOG(INFO,
             "Payload progress callback called. id: %s, "
             "payload_id: %d, bytes transferred: %d, total: %d, status: %d",
             endpoint_id, payload_progress_info.id,
             payload_progress_info.bytes_transferred,
             payload_progress_info.total_bytes, payload_progress_info.status);
  Dart_CObject dart_object_endpoint_id;
  dart_object_endpoint_id.type = Dart_CObject_kString;
  dart_object_endpoint_id.value.as_string = const_cast<char *>(endpoint_id);

  Dart_CObject dart_object_payload_id;
  dart_object_payload_id.type = Dart_CObject_kInt64;
  dart_object_payload_id.value.as_int64 = payload_progress_info.id;

  Dart_CObject dart_object_bytes_transferred;
  dart_object_bytes_transferred.type = Dart_CObject_kInt64;
  dart_object_bytes_transferred.value.as_int64 =
      payload_progress_info.bytes_transferred;

  Dart_CObject dart_object_total_bytes;
  dart_object_total_bytes.type = Dart_CObject_kInt64;
  dart_object_total_bytes.value.as_int64 = payload_progress_info.total_bytes;

  Dart_CObject dart_object_status;
  dart_object_status.type = Dart_CObject_kInt64;
  dart_object_status.value.as_int64 = (int64_t)payload_progress_info.status;

  Dart_CObject *elements[5];
  elements[0] = &dart_object_endpoint_id;
  elements[1] = &dart_object_payload_id;
  elements[2] = &dart_object_bytes_transferred;
  elements[3] = &dart_object_total_bytes;
  elements[4] = &dart_object_status;

  Dart_CObject dart_object_payload_progress;
  dart_object_payload_progress.type = Dart_CObject_kArray;
  dart_object_payload_progress.value.as_array.length = 5;
  dart_object_payload_progress.value.as_array.values = elements;

  if (!Dart_PostCObject_DL(
          current_payload_listener_dart.payload_progress_dart_port,
          &dart_object_payload_progress)) {
    NEARBY_LOG(INFO, "Posting message to port failed.");
  }
}

void PostResult(Dart_Port &result_cb, NC_STATUS value) {
  port = result_cb;
  Dart_CObject dart_object_result_callback;
  dart_object_result_callback.type = Dart_CObject_kInt64;
  dart_object_result_callback.value.as_int64 = static_cast<int64_t>(value);
  const bool result =
      Dart_PostCObject_DL(result_cb, &dart_object_result_callback);
  if (!result) {
    NEARBY_LOG(INFO, "Returning error to port failed.");
  }
}

NC_INSTANCE OpenServiceDart() { return NcOpenService(); }

void CloseServiceDart(NC_INSTANCE instance) { NcCloseService(instance); }

char *GetLocalEndpointIdDart(NC_INSTANCE instance) {
  return NcGetLocalEndpointId(instance);
}

void EnableBleV2Dart(NC_INSTANCE instance, int64_t enable,
                     Dart_Port result_cb) {
  port = result_cb;
  nearby::CountDownLatch finished(1);
  adapter_finished = &finished;
  NcEnableBleV2(instance, enable, [](NC_STATUS status) { ResultCB(status); });
  finished.Await();
  NEARBY_LOGS(INFO) << "EnableBleV2Dart callback is called with enable="
                    << enable;
}

void StartAdvertisingDart(NC_INSTANCE instance, const char *service_id,
                          AdvertisingOptionsDart options_dart,
                          ConnectionRequestInfoDart info_dart,
                          Dart_Port result_cb) {
  if (instance == nullptr) {
    PostResult(result_cb, NC_STATUS_ERROR);
    return;
  }

  port = result_cb;
  current_connection_listener_dart = info_dart.connection_listener;

  NC_ADVERTISING_OPTIONS advertising_options{};
  advertising_options.common_options.strategy.type =
      GetStrategy(options_dart.strategy);
  advertising_options.auto_upgrade_bandwidth =
      options_dart.auto_upgrade_bandwidth;
  advertising_options.enforce_topology_constraints =
      options_dart.enforce_topology_constraints;

  advertising_options.low_power = options_dart.low_power;
  advertising_options.fast_advertisement_service_uuid.data =
      const_cast<char *>(options_dart.fast_advertisement_service_uuid);
  advertising_options.fast_advertisement_service_uuid.size =
      strlen(options_dart.fast_advertisement_service_uuid);

  advertising_options.common_options.allowed_mediums[NC_MEDIUM_BLUETOOTH] =
      options_dart.mediums.bluetooth != 0;
  advertising_options.common_options.allowed_mediums[NC_MEDIUM_BLE] =
      options_dart.mediums.ble != 0;
  advertising_options.common_options.allowed_mediums[NC_MEDIUM_WIFI_LAN] =
      options_dart.mediums.wifi_lan != 0;
  advertising_options.common_options.allowed_mediums[NC_MEDIUM_WIFI_HOTSPOT] =
      options_dart.mediums.wifi_hotspot;
  advertising_options.common_options.allowed_mediums[NC_MEDIUM_WEB_RTC] =
      options_dart.mediums.web_rtc;

  NC_CONNECTION_REQUEST_INFO request_info{};

  request_info.endpoint_info.data = info_dart.endpoint_info;
  request_info.endpoint_info.size = info_dart.endpoint_info_size;
  request_info.initiated_callback = ListenerInitiatedCB;
  request_info.accepted_callback = ListenerAcceptedCB;
  request_info.rejected_callback = ListenerRejectedCB;
  request_info.disconnected_callback = ListenerDisconnectedCB;
  request_info.bandwidth_changed_callback = ListenerBandwidthChangedCB;

  nearby::CountDownLatch finished(1);
  adapter_finished = &finished;

  NcStartAdvertising(instance, service_id, advertising_options, request_info,
                     [](NC_STATUS status) {
                       ResultCB(status);
                     });

  finished.Await();
}

void StopAdvertisingDart(NC_INSTANCE instance, Dart_Port result_cb) {
  if (instance == nullptr) {
    PostResult(result_cb, NC_STATUS_ERROR);
    return;
  }

  port = result_cb;

  nearby::CountDownLatch finished(1);
  adapter_finished = &finished;

  NcStopAdvertising(instance, [](NC_STATUS status) { ResultCB(status); });

  finished.Await();
}

void StartDiscoveryDart(NC_INSTANCE instance, const char *service_id,
                        DiscoveryOptionsDart options_dart,
                        DiscoveryListenerDart listener_dart,
                        Dart_Port result_cb) {
  if (instance == nullptr) {
    PostResult(result_cb, NC_STATUS_ERROR);
    return;
  }

  port = result_cb;
  current_discovery_listener_dart = listener_dart;

  NC_DISCOVERY_OPTIONS discovery_options{};
  discovery_options.common_options.strategy.type =
      GetStrategy(options_dart.strategy);
  discovery_options.enforce_topology_constraints = true;
  // This needs to be passed in by the UI. If it's null, then no
  // fast_advertisement_service. Otherwise this interface will always
  // and forever be locked into 0000FE2C-0000-1000-8000-00805F9B34FB
  // whenever fast advertisement service is requested.
  discovery_options.fast_advertisement_service_uuid.data =
      const_cast<char *>(options_dart.fast_advertisement_service_uuid);
  discovery_options.fast_advertisement_service_uuid.size =
      strlen(options_dart.fast_advertisement_service_uuid);

  discovery_options.common_options.allowed_mediums[NC_MEDIUM_BLUETOOTH] =
      options_dart.mediums.bluetooth != 0;
  discovery_options.common_options.allowed_mediums[NC_MEDIUM_BLE] =
      options_dart.mediums.ble != 0;
  discovery_options.common_options.allowed_mediums[NC_MEDIUM_WIFI_LAN] =
      options_dart.mediums.wifi_lan != 0;
  discovery_options.common_options.allowed_mediums[NC_MEDIUM_WIFI_HOTSPOT] =
      options_dart.mediums.wifi_hotspot;
  discovery_options.common_options.allowed_mediums[NC_MEDIUM_WEB_RTC] =
      options_dart.mediums.web_rtc;

  NC_DISCOVERY_LISTENER listener{};
  listener.endpoint_distance_changed_callback =
      ListenerEndpointDistanceChangedCB;
  listener.endpoint_found_callback = ListenerEndpointFoundCB;
  listener.endpoint_lost_callback = ListenerEndpointLostCB;

  nearby::CountDownLatch finished(1);
  adapter_finished = &finished;

  NcStartDiscovery(instance, service_id, discovery_options, listener,
                   [](NC_STATUS status) {
                     ResultCB(status);
                   });

  finished.Await();
}

void StopDiscoveryDart(NC_INSTANCE instance, Dart_Port result_cb) {
  if (instance == nullptr) {
    PostResult(result_cb, NC_STATUS_ERROR);
    return;
  }

  port = result_cb;

  nearby::CountDownLatch finished(1);
  adapter_finished = &finished;

  NcStopDiscovery(instance, [](NC_STATUS status) { ResultCB(status); });
  adapter_finished->Await();
}

void RequestConnectionDart(NC_INSTANCE instance, const char *endpoint_id,
                           ConnectionOptionsDart options_dart,
                           ConnectionRequestInfoDart info_dart,
                           Dart_Port result_cb) {
  if (instance == nullptr) {
    PostResult(result_cb, NC_STATUS_ERROR);
    return;
  }

  port = result_cb;
  current_connection_listener_dart = info_dart.connection_listener;

  NC_CONNECTION_OPTIONS connection_options;
  connection_options.enforce_topology_constraints =
      options_dart.enforce_topology_constraints;
  connection_options.remote_bluetooth_mac_address.data =
      options_dart.remote_bluetooth_mac_address;
  connection_options.remote_bluetooth_mac_address.size =
      strlen(options_dart.remote_bluetooth_mac_address);
  connection_options.fast_advertisement_service_uuid.data =
      options_dart.fast_advertisement_service_uuid;
  connection_options.fast_advertisement_service_uuid.size =
      strlen(options_dart.fast_advertisement_service_uuid);
  connection_options.keep_alive_interval_millis =
      options_dart.keep_alive_interval_millis;
  connection_options.keep_alive_timeout_millis =
      options_dart.keep_alive_timeout_millis;

  connection_options.common_options.allowed_mediums[NC_MEDIUM_BLUETOOTH] =
      options_dart.mediums.bluetooth != 0;
  connection_options.common_options.allowed_mediums[NC_MEDIUM_BLE] =
      options_dart.mediums.ble != 0;
  connection_options.common_options.allowed_mediums[NC_MEDIUM_WIFI_LAN] =
      options_dart.mediums.wifi_lan != 0;
  connection_options.common_options.allowed_mediums[NC_MEDIUM_WIFI_HOTSPOT] =
      options_dart.mediums.wifi_hotspot;
  connection_options.common_options.allowed_mediums[NC_MEDIUM_WEB_RTC] =
      options_dart.mediums.web_rtc;

  NC_CONNECTION_REQUEST_INFO request_info{};

  request_info.endpoint_info.data = info_dart.endpoint_info;
  request_info.endpoint_info.size = info_dart.endpoint_info_size;
  request_info.initiated_callback = ListenerInitiatedCB;
  request_info.accepted_callback = ListenerAcceptedCB;
  request_info.rejected_callback = ListenerRejectedCB;
  request_info.disconnected_callback = ListenerDisconnectedCB;
  request_info.bandwidth_changed_callback = ListenerBandwidthChangedCB;

  nearby::CountDownLatch finished(1);
  adapter_finished = &finished;

  NcRequestConnection(instance, endpoint_id, request_info, connection_options,
                      [](NC_STATUS status) { ResultCB(status); });

  adapter_finished->Await();
}

void AcceptConnectionDart(NC_INSTANCE instance, const char *endpoint_id,
                          PayloadListenerDart listener_dart,
                          Dart_Port result_cb) {
  if (instance == nullptr) {
    PostResult(result_cb, NC_STATUS_ERROR);
    return;
  }

  port = result_cb;
  current_payload_listener_dart = listener_dart;

  NC_PAYLOAD_LISTENER listener{};
  listener.received_callback = ListenerPayloadCB;
  listener.progress_updated_callback = ListenerPayloadProgressCB;

  nearby::CountDownLatch finished(1);
  adapter_finished = &finished;

  NcAcceptConnection(instance, endpoint_id, listener,
                     [](NC_STATUS status) { ResultCB(status); });

  finished.Await();
}

void RejectConnectionDart(NC_INSTANCE instance, const char *endpoint_id,
                          Dart_Port result_cb) {
  if (instance == nullptr) {
    PostResult(result_cb, NC_STATUS_ERROR);
    return;
  }

  port = result_cb;

  nearby::CountDownLatch finished(1);
  adapter_finished = &finished;

  NcRejectConnection(instance, endpoint_id,
                     [](NC_STATUS status) { ResultCB(status); });

  finished.Await();
}

void DisconnectFromEndpointDart(NC_INSTANCE instance, char *endpoint_id,
                                Dart_Port result_cb) {
  if (instance == nullptr) {
    PostResult(result_cb, NC_STATUS_ERROR);
    return;
  }

  port = result_cb;

  nearby::CountDownLatch finished(1);
  adapter_finished = &finished;

  NcDisconnectFromEndpoint(instance, endpoint_id,
                           [](NC_STATUS status) { ResultCB(status); });

  finished.Await();
}

void SendPayloadDart(NC_INSTANCE instance, const char *endpoint_id,
                     PayloadDart payload_dart, Dart_Port result_cb) {
  if (instance == nullptr) {
    PostResult(result_cb, NC_STATUS_ERROR);
    return;
  }

  port = result_cb;

  std::vector<std::string> endpoint_ids = {std::string(endpoint_id)};

  NEARBY_LOG(INFO, "Payload type: %d", payload_dart.type);
  switch (payload_dart.type) {
    case PAYLOAD_TYPE_UNKNOWN:
    case PAYLOAD_TYPE_STREAM:
      NEARBY_LOG(INFO, "Payload type not supported yet");
      PostResult(port, NC_STATUS_PAYLOADUNKNOWN);
      break;
    case PAYLOAD_TYPE_BYTE: {
      NC_PAYLOAD payload{};
      payload.id = GeneratePayloadId();
      payload.type = NC_PAYLOAD_TYPE_BYTES;
      payload.direction = NC_PAYLOAD_DIRECTION_INCOMING;
      payload.content.bytes.content.data = payload_dart.data;
      payload.content.bytes.content.size = payload_dart.size;

      std::vector<const char *> c_string_array;

      std::transform(endpoint_ids.begin(), endpoint_ids.end(),
                     std::back_inserter(c_string_array),
                     [](const std::string &s) {
                       char *pc = new char[s.size() + 1];
                       strncpy(pc, s.c_str(), s.size() + 1);
                       return pc;
                     });

      nearby::CountDownLatch finished(1);
      adapter_finished = &finished;

      NcSendPayload(instance, c_string_array.size(), c_string_array.data(),
                    std::move(payload),
                    [](NC_STATUS status) { ResultCB(status); });

      adapter_finished->Await();
      break;
    }
    case PAYLOAD_TYPE_FILE:
      NEARBY_LOG(INFO, "File name: %s, size %d", payload_dart.data,
                 payload_dart.size);
      std::string file_name_str(payload_dart.data);

      NC_PAYLOAD payload{};
      payload.id = GeneratePayloadId();
      payload.type = NC_PAYLOAD_TYPE_FILE;
      payload.direction = NC_PAYLOAD_DIRECTION_INCOMING;
      payload.content.file.file_name =
          const_cast<char *>(file_name_str.c_str());
      payload.content.file.parent_folder = nullptr;

      std::vector<const char *> c_string_array;

      std::transform(endpoint_ids.begin(), endpoint_ids.end(),
                     std::back_inserter(c_string_array),
                     [](const std::string &s) {
                       char *pc = new char[s.size() + 1];
                       strncpy(pc, s.c_str(), s.size() + 1);
                       return pc;
                     });

      nearby::CountDownLatch finished(1);
      adapter_finished = &finished;

      NcSendPayload(instance, c_string_array.size(), c_string_array.data(),
                    std::move(payload),
                    [](NC_STATUS status) { ResultCB(status); });

      adapter_finished->Await();

      break;
  }
}
