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

  final StreamController<Map<String, dynamic>> _controlController =
      StreamController<Map<String, dynamic>>.broadcast();
  final StreamController<void> _configRequestedController =
      StreamController<void>.broadcast();
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
      if (map['action'] == 'request_config') {
        _configRequestedController.add(null);
      }
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

  @override
  Stream<void> get onConfigRequested {
    _ensureHandler();
    return _configRequestedController.stream;
  }

  @override
  Future<void> setAudioVolume(String channel, int volume) async {
    await methodChannel.invokeMethod<void>('setAudioVolume', <String, dynamic>{
      'channel': channel,
      'volume': volume,
    });
  }

  @override
  Future<void> setAudioDevice(String device) async {
    await methodChannel.invokeMethod<void>('setAudioDevice', <String, dynamic>{
      'device': device,
    });
  }

  @override
  Future<List<Map<String, String>>> getAudioDevices() async {
    final result = await methodChannel.invokeMethod<List<dynamic>>('getAudioDevices');
    if (result == null) return [];
    return result.map((e) {
      final map = Map<String, dynamic>.from(e as Map);
      return map.map((k, v) => MapEntry(k, v.toString()));
    }).toList();
  }
}
