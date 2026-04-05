
import 'dart:async';
import 'dart:convert';

import 'openautoflutter_platform_interface.dart';

/// Touch actions mirrored on native side.
enum TouchAction {
  down,
  up,
  moved,
  pointerDown,
  pointerUp,
}

extension TouchActionCode on TouchAction {
  int get code {
    switch (this) {
      case TouchAction.down:
        return 0;
      case TouchAction.up:
        return 1;
      case TouchAction.moved:
        return 2;
      case TouchAction.pointerDown:
        return 3;
      case TouchAction.pointerUp:
        return 4;
    }
  }
}

class Openautoflutter {
  Future<String?> getPlatformVersion() {
    return OpenautoflutterPlatform.instance.getPlatformVersion();
  }

  Future<int?> getVideoTextureId() {
    return OpenautoflutterPlatform.instance.getVideoTextureId();
  }

  Future<void> sendTouchEvent({
    required int pointerId,
    required double x,
    required double y,
    required TouchAction action,
  }) {
    return OpenautoflutterPlatform.instance.sendTouchEvent(
      pointerId: pointerId,
      x: x,
      y: y,
      actionCode: action.code,
    );
  }

  /// Sends a JSON string for one of the supported sensor payloads:
  ///
  /// Location:
  /// {
  ///   "location": {
  ///     "latitude": 37.7749,
  ///     "longitude": -122.4194,
  ///     "accuracy_m": 5.0,
  ///     "altitude_m": 15.0,
  ///     "speed_mps": 13.4,
  ///     "bearing_deg": 90.0
  ///   }
  /// }
  /// Required: latitude, longitude
  /// Optional: accuracy_m, altitude_m, speed_mps, bearing_deg
  ///
  /// Night mode:
  /// { "night_mode": { "enabled": true } }
  /// Required: enabled (boolean)
  ///
  /// Driving status:
  /// { "driving_status": { "status": "no_video" } }
  /// Required: status (string)
  /// Allowed values: unrestricted, no_video, no_keyboard_input,
  ///   no_voice_input, no_config, limit_message_len
  Future<void> sendSensorJson(String json) {
    return OpenautoflutterPlatform.instance.sendSensorJson(json);
  }

  /// Requests the current config from the core (side A) over transport.
  ///
  /// Sends a `{"action": "get"}` message over `MsgType::CONFIGURATION`
  /// and waits for the core to respond with the current config JSON.
  /// If the core does not respond within 5 seconds, returns `null`.
  ///
  /// Returns the parsed JSON map, or `null` on timeout / error.
  Future<Map<String, dynamic>?> getConfig() {
    return OpenautoflutterPlatform.instance.getConfig();
  }

  /// Replaces the running config on the core (side A).
  ///
  /// [config] must match the schema of
  /// `ServiceDiscoveryResponse.default.json`. The core validates the
  /// JSON by constructing the protobuf; if validation fails the request
  /// is silently dropped.
  ///
  /// Fire-and-forget — there is no response.
  Future<void> sendConfigSet(Map<String, dynamic> config) {
    final request = jsonEncode({'action': 'set', 'config': config});
    return OpenautoflutterPlatform.instance.sendConfigJson(request);
  }

  /// Deletes the user overlay and reloads the shipped default on the
  /// core (side A).
  ///
  /// Fire-and-forget — there is no response.
  Future<void> sendConfigReset() {
    final request = jsonEncode({'action': 'reset'});
    return OpenautoflutterPlatform.instance.sendConfigJson(request);
  }

  // ── Device control ──────────────────────────────────────────────────

  /// Stream of control messages from the core (e.g. device_list updates).
  Stream<Map<String, dynamic>> get onControlReceived {
    return OpenautoflutterPlatform.instance.onControlReceived;
  }

  /// Requests the current list of available devices from the core.
  ///
  /// The core will respond asynchronously via [onControlReceived] with
  /// `{"action": "device_list", "devices": [...]}`.
  Future<void> requestDevices() {
    final request = jsonEncode({'action': 'get_devices'});
    return OpenautoflutterPlatform.instance.sendControlJson(request);
  }

  /// Tells the core to connect to the device with the given [id].
  ///
  /// [id] is the device identifier from the device list (e.g. "usb:18d1:2d01").
  /// The core sets the device status to "connected" and broadcasts
  /// an updated device_list.
  Future<void> connectDevice(String id) {
    final request = jsonEncode({'action': 'connect_device', 'id': id});
    return OpenautoflutterPlatform.instance.sendControlJson(request);
  }

  /// Tells the core to disconnect the device with the given [id].
  ///
  /// The core stops the AA session and marks the device back as
  /// "available", then broadcasts an updated device_list.
  Future<void> disconnectDevice(String id) {
    final request = jsonEncode({'action': 'disconnect_device', 'id': id});
    return OpenautoflutterPlatform.instance.sendControlJson(request);
  }
}
