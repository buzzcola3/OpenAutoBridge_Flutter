import 'package:plugin_platform_interface/plugin_platform_interface.dart';

import 'open_auto_bridge_flutter_method_channel.dart';

abstract class OpenAutoBridgePlatform extends PlatformInterface {
  /// Constructs a OpenAutoBridgePlatform.
  OpenAutoBridgePlatform() : super(token: _token);

  static final Object _token = Object();

  static OpenAutoBridgePlatform _instance = MethodChannelOpenAutoBridge();

  /// The default instance of [OpenAutoBridgePlatform] to use.
  ///
  /// Defaults to [MethodChannelOpenAutoBridge].
  static OpenAutoBridgePlatform get instance => _instance;

  /// Platform-specific implementations should set this with their own
  /// platform-specific class that extends [OpenAutoBridgePlatform] when
  /// they register themselves.
  static set instance(OpenAutoBridgePlatform instance) {
    PlatformInterface.verifyToken(instance, _token);
    _instance = instance;
  }

  Future<String?> getPlatformVersion() {
    throw UnimplementedError('platformVersion() has not been implemented.');
  }

  Future<int?> getVideoTextureId() {
    throw UnimplementedError('getVideoTextureId() has not been implemented.');
  }

  Future<void> sendTouchEvent({
    required int pointerId,
    required double x,
    required double y,
    required int actionCode,
  }) {
    throw UnimplementedError('sendTouchEvent() has not been implemented.');
  }

  Future<void> sendSensorJson(String json) {
    throw UnimplementedError('sendSensorJson() has not been implemented.');
  }

  Future<void> sendConfigJson(String json) {
    throw UnimplementedError('sendConfigJson() has not been implemented.');
  }

  Future<Map<String, dynamic>?> getConfig() {
    throw UnimplementedError('getConfig() has not been implemented.');
  }

  Future<void> sendControlJson(String json) {
    throw UnimplementedError('sendControlJson() has not been implemented.');
  }

  /// Stream of control messages received from the core.
  Stream<Map<String, dynamic>> get onControlReceived {
    throw UnimplementedError('onControlReceived has not been implemented.');
  }
}
