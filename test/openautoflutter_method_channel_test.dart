import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:openautoflutter/openautoflutter_method_channel.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  MethodChannelOpenautoflutter platform = MethodChannelOpenautoflutter();
  const MethodChannel channel = MethodChannel('openautoflutter');
  MethodCall? lastCall;

  setUp(() {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger.setMockMethodCallHandler(
      channel,
      (MethodCall methodCall) async {
        lastCall = methodCall;
        if (methodCall.method == 'getPlatformVersion') {
          return '42';
        }
        if (methodCall.method == 'getVideoTextureId') {
          return 7;
        }
        return null;
      },
    );
  });

  tearDown(() {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger.setMockMethodCallHandler(channel, null);
  });

  test('getPlatformVersion', () async {
    expect(await platform.getPlatformVersion(), '42');
  });

  test('getVideoTextureId', () async {
    expect(await platform.getVideoTextureId(), 7);
  });

  test('sendSensorJson', () async {
    await platform.sendSensorJson('{"hello":"world"}');
    expect(lastCall?.method, 'sendSensorJson');
    expect(lastCall?.arguments, <String, dynamic>{'json': '{"hello":"world"}'});
  });
}
