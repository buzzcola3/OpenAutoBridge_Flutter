import 'dart:async';
import 'dart:convert';

import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';

import 'open_auto_bridge_flutter_platform_interface.dart';

/// An implementation of [OpenAutoBridgePlatform] that uses method channels.
class MethodChannelOpenAutoBridge extends OpenAutoBridgePlatform {
  /// The method channel used to interact with the native platform.
  @visibleForTesting
  final methodChannel = const MethodChannel('open_auto_bridge_flutter');

  Completer<Map<String, dynamic>?>? _configCompleter;
  final StreamController<Map<String, dynamic>> _controlController =
      StreamController<Map<String, dynamic>>.broadcast();
  bool _handlerInstalled = false;

  void _ensureHandler() {
    if (_handlerInstalled) return;
    _handlerInstalled = true;
    methodChannel.setMethodCallHandler(_handleNativeCall);
  }

  Future<dynamic> _handleNativeCall(MethodCall call) async {
    if (call.method == 'onConfigReceived') {
      final json = call.arguments as String;
      final map = jsonDecode(json) as Map<String, dynamic>;
      _configCompleter?.complete(map);
      _configCompleter = null;
    } else if (call.method == 'onControlReceived') {
      final json = call.arguments as String;
      final map = jsonDecode(json) as Map<String, dynamic>;
      _controlController.add(map);
    }
  }

  @override
  Future<String?> getPlatformVersion() async {
    final version = await methodChannel.invokeMethod<String>('getPlatformVersion');
    return version;
  }

  @override
  Future<int?> getVideoTextureId() async {
    final id = await methodChannel.invokeMethod<int>('getVideoTextureId');
    return id;
  }

  @override
  Future<void> sendTouchEvent({
    required int pointerId,
    required double x,
    required double y,
    required int actionCode,
  }) async {
    await methodChannel.invokeMethod<void>('sendTouchEvent', <String, dynamic>{
      'pointerId': pointerId,
      'x': x,
      'y': y,
      'action': actionCode,
    });
  }

  @override
  Future<void> sendSensorJson(String json) async {
    await methodChannel.invokeMethod<void>('sendSensorJson', <String, dynamic>{
      'json': json,
    });
  }

  @override
  Future<void> sendConfigJson(String json) async {
    await methodChannel.invokeMethod<void>('sendConfigJson', <String, dynamic>{
      'json': json,
    });
  }

  @override
  Future<Map<String, dynamic>?> getConfig() async {
    _ensureHandler();
    _configCompleter = Completer<Map<String, dynamic>?>();
    await sendConfigJson(jsonEncode({'action': 'get'}));
    return _configCompleter!.future.timeout(
      const Duration(seconds: 5),
      onTimeout: () {
        _configCompleter = null;
        return null;
      },
    );
  }

  @override
  Future<void> sendControlJson(String json) async {
    await methodChannel.invokeMethod<void>('sendControlJson', <String, dynamic>{
      'json': json,
    });
  }

  @override
  Stream<Map<String, dynamic>> get onControlReceived {
    _ensureHandler();
    return _controlController.stream;
  }
}
