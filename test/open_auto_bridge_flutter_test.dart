import 'dart:async';

import 'package:flutter_test/flutter_test.dart';
import 'package:open_auto_bridge_flutter/open_auto_bridge_flutter.dart';
import 'package:open_auto_bridge_flutter/open_auto_bridge_flutter_platform_interface.dart';
import 'package:open_auto_bridge_flutter/open_auto_bridge_flutter_method_channel.dart';
import 'package:plugin_platform_interface/plugin_platform_interface.dart';

class MockOpenAutoBridgePlatform
    with MockPlatformInterfaceMixin
    implements OpenAutoBridgePlatform {

  @override
  Future<String?> getPlatformVersion() => Future.value('42');

  @override
  Future<int?> getVideoTextureId() => Future.value(7);

  @override
  Future<void> sendTouchEvent({
    required int pointerId,
    required double x,
    required double y,
    required int actionCode,
  }) {
    return Future.value();
  }

  @override
  Future<void> sendSensorJson(String json) {
    return Future.value();
  }

  @override
  Future<void> sendConfigJson(String json) {
    return Future.value();
  }

  @override
  Future<Map<String, dynamic>?> getConfig() {
    return Future.value({'test': true});
  }

  @override
  Future<void> sendControlJson(String json) {
    return Future.value();
  }

  final _controlController = StreamController<Map<String, dynamic>>.broadcast();

  @override
  Stream<Map<String, dynamic>> get onControlReceived => _controlController.stream;
}

void main() {
  final OpenAutoBridgePlatform initialPlatform = OpenAutoBridgePlatform.instance;

  test('$MethodChannelOpenAutoBridge is the default instance', () {
    expect(initialPlatform, isInstanceOf<MethodChannelOpenAutoBridge>());
  });

  test('getPlatformVersion', () async {
    OpenAutoBridge open_auto_bridge_flutterPlugin = OpenAutoBridge();
    MockOpenAutoBridgePlatform fakePlatform = MockOpenAutoBridgePlatform();
    OpenAutoBridgePlatform.instance = fakePlatform;

    expect(await open_auto_bridge_flutterPlugin.getPlatformVersion(), '42');
  });
}
