
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
}
