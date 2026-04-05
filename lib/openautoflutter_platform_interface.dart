import 'package:plugin_platform_interface/plugin_platform_interface.dart';

import 'openautoflutter_method_channel.dart';

abstract class OpenautoflutterPlatform extends PlatformInterface {
  /// Constructs a OpenautoflutterPlatform.
  OpenautoflutterPlatform() : super(token: _token);

  static final Object _token = Object();

  static OpenautoflutterPlatform _instance = MethodChannelOpenautoflutter();

  /// The default instance of [OpenautoflutterPlatform] to use.
  ///
  /// Defaults to [MethodChannelOpenautoflutter].
  static OpenautoflutterPlatform get instance => _instance;

  /// Platform-specific implementations should set this with their own
  /// platform-specific class that extends [OpenautoflutterPlatform] when
  /// they register themselves.
  static set instance(OpenautoflutterPlatform instance) {
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
