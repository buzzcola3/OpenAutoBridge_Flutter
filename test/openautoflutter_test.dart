import 'package:flutter_test/flutter_test.dart';
import 'package:openautoflutter/openautoflutter.dart';
import 'package:openautoflutter/openautoflutter_platform_interface.dart';
import 'package:openautoflutter/openautoflutter_method_channel.dart';
import 'package:plugin_platform_interface/plugin_platform_interface.dart';

class MockOpenautoflutterPlatform
    with MockPlatformInterfaceMixin
    implements OpenautoflutterPlatform {

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
}

void main() {
  final OpenautoflutterPlatform initialPlatform = OpenautoflutterPlatform.instance;

  test('$MethodChannelOpenautoflutter is the default instance', () {
    expect(initialPlatform, isInstanceOf<MethodChannelOpenautoflutter>());
  });

  test('getPlatformVersion', () async {
    Openautoflutter openautoflutterPlugin = Openautoflutter();
    MockOpenautoflutterPlatform fakePlatform = MockOpenautoflutterPlatform();
    OpenautoflutterPlatform.instance = fakePlatform;

    expect(await openautoflutterPlugin.getPlatformVersion(), '42');
  });
}
