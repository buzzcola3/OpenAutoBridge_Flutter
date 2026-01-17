
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

  /// Sends a JSON string for sensor location data with the structure:
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
  Future<void> sendSensorJson(String json) {
    return OpenautoflutterPlatform.instance.sendSensorJson(json);
  }
}
