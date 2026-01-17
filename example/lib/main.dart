import 'package:flutter/material.dart';
import 'dart:async';
import 'dart:convert';

import 'package:flutter/services.dart';
import 'package:openautoflutter/openautoflutter.dart';

void main() {
  runApp(const MyApp());
}

class MyApp extends StatefulWidget {
  const MyApp({super.key});

  @override
  State<MyApp> createState() => _MyAppState();
}

class _MyAppState extends State<MyApp> {
  String _platformVersion = 'Unknown';
  final _openautoflutterPlugin = Openautoflutter();
  int? _videoTextureId;
  final Set<int> _activePointers = <int>{};
  Size? _textureSize;

  @override
  void initState() {
    super.initState();
    initPlatformState();
  }

  // Platform messages are asynchronous, so we initialize in an async method.
  Future<void> initPlatformState() async {
    String platformVersion;
  int? textureId;
    // Platform messages may fail, so we use a try/catch PlatformException.
    // We also handle the message potentially returning null.
    try {
      platformVersion =
          await _openautoflutterPlugin.getPlatformVersion() ?? 'Unknown platform version';
  textureId = await _openautoflutterPlugin.getVideoTextureId();
    } on PlatformException {
      platformVersion = 'Failed to get platform version.';
  textureId = null;
    }

    // If the widget was removed from the tree while the asynchronous platform
    // message was in flight, we want to discard the reply rather than calling
    // setState to update our non-existent appearance.
    if (!mounted) return;

    setState(() {
      _platformVersion = platformVersion;
  _videoTextureId = textureId;
    });
  }

  void _sendTouch(PointerEvent event, TouchAction action) {
    final size = _textureSize;
    if (size == null || size.width == 0 || size.height == 0) return;

    final double xNorm = (event.localPosition.dx / size.width).clamp(0.0, 1.0);
    final double yNorm = (event.localPosition.dy / size.height).clamp(0.0, 1.0);

    _openautoflutterPlugin.sendTouchEvent(
      pointerId: event.pointer,
      x: xNorm,
      y: yNorm,
      action: action,
    );
  }

  Future<void> _sendSensorSample() async {
    final payload = jsonEncode({
      'location': {
        'latitude': 37.7749,
        'longitude': -122.4194,
        'accuracy_m': 5.0,
        'altitude_m': 15.0,
        'speed_mps': 13.4,
        'bearing_deg': 90.0,
      },
    });
    await _openautoflutterPlugin.sendSensorJson(payload);
  }

  Future<void> _sendNightModeSample() async {
    final payload = jsonEncode({
      'night_mode': {
        'enabled': true,
      },
    });
    await _openautoflutterPlugin.sendSensorJson(payload);
  }

  Future<void> _sendDrivingStatusSample() async {
    final payload = jsonEncode({
      'driving_status': {
        'status': 'no_video',
      },
    });
    await _openautoflutterPlugin.sendSensorJson(payload);
  }

  void _handlePointerDown(PointerDownEvent event) {
    final bool isFirst = _activePointers.isEmpty;
    _activePointers.add(event.pointer);
    _sendTouch(event, isFirst ? TouchAction.down : TouchAction.pointerDown);
  }

  void _handlePointerMove(PointerMoveEvent event) {
    if (!_activePointers.contains(event.pointer)) return;
    _sendTouch(event, TouchAction.moved);
  }

  void _handlePointerUp(PointerUpEvent event) {
    final bool isLast = _activePointers.length <= 1;
    _sendTouch(event, isLast ? TouchAction.up : TouchAction.pointerUp);
    _activePointers.remove(event.pointer);
  }

  void _handlePointerCancel(PointerCancelEvent event) {
    final bool isLast = _activePointers.length <= 1;
    _sendTouch(event, isLast ? TouchAction.up : TouchAction.pointerUp);
    _activePointers.remove(event.pointer);
  }

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      home: Scaffold(
        appBar: AppBar(
          title: const Text('Plugin example app'),
        ),
        body: Center(
          child: Padding(
            padding: const EdgeInsets.all(16),
            child: Column(
              mainAxisSize: MainAxisSize.min,
              crossAxisAlignment: CrossAxisAlignment.center,
              children: [
                Text('Running on: $_platformVersion'),
                const SizedBox(height: 12),
                if (_videoTextureId == null)
                  const Text('Texture not available')
                else ...[
                  Text('Texture ID: $_videoTextureId'),
                  const SizedBox(height: 8),
                  ElevatedButton(
                    onPressed: _sendSensorSample,
                    child: const Text('Send sensor JSON'),
                  ),
                  const SizedBox(height: 8),
                  ElevatedButton(
                    onPressed: _sendNightModeSample,
                    child: const Text('Send night mode JSON'),
                  ),
                  const SizedBox(height: 8),
                  ElevatedButton(
                    onPressed: _sendDrivingStatusSample,
                    child: const Text('Send driving status JSON'),
                  ),
                  const SizedBox(height: 8),
                  // Render the native GL video texture with flex to avoid overflow.
                  Flexible(
                    child: LayoutBuilder(
                      builder: (context, constraints) {
                        final double maxWidth = constraints.maxWidth;
                        final double maxHeight = constraints.maxHeight;
                        const double aspect = 16 / 9;

                        double width = maxWidth;
                        double height = width / aspect;
                        if (height > maxHeight) {
                          height = maxHeight;
                          width = height * aspect;
                        }
                        _textureSize = Size(width, height);

                        return Center(
                          child: SizedBox(
                            width: width,
                            height: height,
                            child: Listener(
                              onPointerDown: _handlePointerDown,
                              onPointerMove: _handlePointerMove,
                              onPointerUp: _handlePointerUp,
                              onPointerCancel: _handlePointerCancel,
                              child: Texture(textureId: _videoTextureId!),
                            ),
                          ),
                        );
                      },
                    ),
                  ),
                ],
              ],
            ),
          ),
        ),
      ),
    );
  }
}
