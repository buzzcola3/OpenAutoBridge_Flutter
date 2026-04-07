import 'package:flutter/material.dart';
import 'dart:async';
import 'dart:convert';

import 'package:flutter/services.dart';
import 'package:open_auto_bridge_flutter/open_auto_bridge_flutter.dart';

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
  final _openAutoBridgePlugin = OpenAutoBridge();
  int? _videoTextureId;
  final Set<int> _activePointers = <int>{};
  final Map<int, int> _pointerIdMap = {}; // Flutter pointer → sequential ID
  int _nextPointerId = 0;
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
          await _openAutoBridgePlugin.getPlatformVersion() ?? 'Unknown platform version';
  textureId = await _openAutoBridgePlugin.getVideoTextureId();
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

  int _mapPointerId(int flutterPointer) {
    return _pointerIdMap.putIfAbsent(flutterPointer, () => _nextPointerId++);
  }

  void _sendTouch(PointerEvent event, TouchAction action) {
    final size = _textureSize;
    if (size == null || size.width == 0 || size.height == 0) return;

    final double xNorm = (event.localPosition.dx / size.width).clamp(0.0, 1.0);
    final double yNorm = (event.localPosition.dy / size.height).clamp(0.0, 1.0);

    _openAutoBridgePlugin.sendTouchEvent(
      pointerId: _mapPointerId(event.pointer),
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
    await _openAutoBridgePlugin.sendSensorJson(payload);
  }

  Future<void> _sendNightModeSample() async {
    final payload = jsonEncode({
      'night_mode': {
        'enabled': true,
      },
    });
    await _openAutoBridgePlugin.sendSensorJson(payload);
  }

  Future<void> _sendDrivingStatusSample() async {
    final payload = jsonEncode({
      'driving_status': {
        'status': 'no_video',
      },
    });
    await _openAutoBridgePlugin.sendSensorJson(payload);
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
    _pointerIdMap.remove(event.pointer);
    if (_activePointers.isEmpty) {
      _pointerIdMap.clear();
      _nextPointerId = 0;
    }
  }

  void _handlePointerCancel(PointerCancelEvent event) {
    // Treat cancel as all fingers lifted
    _sendTouch(event, TouchAction.up);
    _activePointers.clear();
    _pointerIdMap.clear();
    _nextPointerId = 0;
  }

  void _openConfigPage() {
    Navigator.of(context).push(
      MaterialPageRoute(
        builder: (_) => ConfigPage(plugin: _openAutoBridgePlugin),
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      home: Builder(
        builder: (context) {
          return Scaffold(
            appBar: AppBar(
              title: const Text('Plugin example app'),
              actions: [
                IconButton(
                  icon: const Icon(Icons.usb),
                  tooltip: 'Devices',
                  onPressed: () {
                    Navigator.of(context).push(
                      MaterialPageRoute(
                        builder: (_) => DevicesPage(plugin: _openAutoBridgePlugin),
                      ),
                    );
                  },
                ),
                IconButton(
                  icon: const Icon(Icons.settings),
                  tooltip: 'Service Config',
                  onPressed: () {
                    Navigator.of(context).push(
                      MaterialPageRoute(
                        builder: (_) => ConfigPage(plugin: _openAutoBridgePlugin),
                      ),
                    );
                  },
                ),
              ],
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
          );
        },
      ),
    );
  }
}

// ---------------------------------------------------------------------------
// Configuration page
// ---------------------------------------------------------------------------

class ConfigPage extends StatefulWidget {
  const ConfigPage({super.key, required this.plugin});
  final OpenAutoBridge plugin;

  @override
  State<ConfigPage> createState() => _ConfigPageState();
}

class _ConfigPageState extends State<ConfigPage> {
  Map<String, dynamic>? _config;
  bool _loading = true;
  String? _status;

  // Editable top-level fields
  late TextEditingController _displayNameCtrl;
  String _driverPosition = 'DRIVER_POSITION_LEFT';
  bool _canPlayNativeMedia = false;

  // Video channel (id 3)
  String _codecResolution = 'VIDEO_800x480';
  String _frameRate = 'VIDEO_FPS_30';
  late TextEditingController _densityCtrl;

  // Headunit info
  late TextEditingController _huMakeCtrl;
  late TextEditingController _huModelCtrl;
  late TextEditingController _huYearCtrl;
  late TextEditingController _huSoftwareVersionCtrl;

  static const _resolutions = [
    'VIDEO_800x480',
    'VIDEO_1280x720',
    'VIDEO_1920x1080',
  ];
  static const _frameRates = ['VIDEO_FPS_30', 'VIDEO_FPS_60'];
  static const _driverPositions = [
    'DRIVER_POSITION_LEFT',
    'DRIVER_POSITION_RIGHT',
  ];

  @override
  void initState() {
    super.initState();
    _displayNameCtrl = TextEditingController();
    _densityCtrl = TextEditingController();
    _huMakeCtrl = TextEditingController();
    _huModelCtrl = TextEditingController();
    _huYearCtrl = TextEditingController();
    _huSoftwareVersionCtrl = TextEditingController();
    _loadConfig();
  }

  @override
  void dispose() {
    _displayNameCtrl.dispose();
    _densityCtrl.dispose();
    _huMakeCtrl.dispose();
    _huModelCtrl.dispose();
    _huYearCtrl.dispose();
    _huSoftwareVersionCtrl.dispose();
    super.dispose();
  }

  Future<void> _loadConfig() async {
    final config = await widget.plugin.getConfig();
    if (!mounted) return;
    setState(() {
      _loading = false;
      _config = config;
      if (config != null) _populateFields(config);
    });
  }

  void _populateFields(Map<String, dynamic> config) {
    _displayNameCtrl.text = config['display_name'] ?? '';
    _driverPosition = config['driver_position'] ?? 'DRIVER_POSITION_LEFT';
    _canPlayNativeMedia = config['can_play_native_media_during_vr'] ?? false;

    // Find video channel (id 3)
    final channels = config['channels'] as List<dynamic>? ?? [];
    for (final ch in channels) {
      if (ch['id'] == 3 && ch['media_sink_service'] != null) {
        final sink = ch['media_sink_service'] as Map<String, dynamic>;
        final videoConfigs = sink['video_configs'] as List<dynamic>? ?? [];
        if (videoConfigs.isNotEmpty) {
          final vc = videoConfigs[0] as Map<String, dynamic>;
          _codecResolution = vc['codec_resolution'] ?? _codecResolution;
          _frameRate = vc['frame_rate'] ?? _frameRate;
          _densityCtrl.text = (vc['density'] ?? 140).toString();
        }
        break;
      }
    }

    // Headunit info
    final hu = config['headunit_info'] as Map<String, dynamic>? ?? {};
    _huMakeCtrl.text = hu['make'] ?? '';
    _huModelCtrl.text = hu['model'] ?? '';
    _huYearCtrl.text = hu['year'] ?? '';
    _huSoftwareVersionCtrl.text = hu['head_unit_software_version'] ?? '';
  }

  Map<String, dynamic> _buildConfig() {
    final config = _config != null
        ? Map<String, dynamic>.from(
            jsonDecode(jsonEncode(_config)) as Map<String, dynamic>)
        : <String, dynamic>{};

    config['display_name'] = _displayNameCtrl.text;
    config['driver_position'] = _driverPosition;
    config['can_play_native_media_during_vr'] = _canPlayNativeMedia;

    // Update video channel
    final channels = (config['channels'] as List<dynamic>? ?? [])
        .map((e) => Map<String, dynamic>.from(e as Map))
        .toList();
    for (final ch in channels) {
      if (ch['id'] == 3 && ch['media_sink_service'] != null) {
        final sink = Map<String, dynamic>.from(
            ch['media_sink_service'] as Map);
        final videoConfigs =
            (sink['video_configs'] as List<dynamic>? ?? [])
                .map((e) => Map<String, dynamic>.from(e as Map))
                .toList();
        if (videoConfigs.isNotEmpty) {
          videoConfigs[0]['codec_resolution'] = _codecResolution;
          videoConfigs[0]['frame_rate'] = _frameRate;
          videoConfigs[0]['density'] =
              int.tryParse(_densityCtrl.text) ?? 140;
        }
        sink['video_configs'] = videoConfigs;
        ch['media_sink_service'] = sink;
        break;
      }
    }
    config['channels'] = channels;

    // Update headunit info
    final hu = Map<String, dynamic>.from(
        config['headunit_info'] as Map? ?? {});
    hu['make'] = _huMakeCtrl.text;
    hu['model'] = _huModelCtrl.text;
    hu['year'] = _huYearCtrl.text;
    hu['head_unit_software_version'] = _huSoftwareVersionCtrl.text;
    config['headunit_info'] = hu;

    return config;
  }

  Future<void> _sendConfig() async {
    final config = _buildConfig();
    await widget.plugin.sendConfigSet(config);
    if (!mounted) return;
    setState(() => _status = 'Config sent');
  }

  Future<void> _resetConfig() async {
    await widget.plugin.sendConfigReset();
    if (!mounted) return;
    setState(() => _status = 'Config reset sent');
    _loadConfig();
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('Service Config'),
        actions: [
          IconButton(
            icon: const Icon(Icons.restore),
            tooltip: 'Reset to default',
            onPressed: _resetConfig,
          ),
        ],
      ),
      body: _loading
          ? const Center(child: CircularProgressIndicator())
          : _config == null
              ? const Center(child: Text('No config file found'))
              : _buildForm(),
      floatingActionButton: _config != null
          ? FloatingActionButton.extended(
              onPressed: _sendConfig,
              icon: const Icon(Icons.send),
              label: const Text('Apply'),
            )
          : null,
    );
  }

  Widget _buildForm() {
    return ListView(
      padding: const EdgeInsets.all(16),
      children: [
        if (_status != null) ...[
          Text(_status!, style: TextStyle(color: Colors.green[700])),
          const SizedBox(height: 12),
        ],

        // --- General ---
        _sectionHeader('General'),
        TextField(
          controller: _displayNameCtrl,
          decoration: const InputDecoration(labelText: 'Display Name'),
        ),
        const SizedBox(height: 8),
        DropdownButtonFormField<String>(
          value: _driverPositions.contains(_driverPosition)
              ? _driverPosition
              : _driverPositions.first,
          decoration: const InputDecoration(labelText: 'Driver Position'),
          items: _driverPositions
              .map((v) => DropdownMenuItem(value: v, child: Text(v)))
              .toList(),
          onChanged: (v) => setState(() => _driverPosition = v!),
        ),
        const SizedBox(height: 8),
        SwitchListTile(
          title: const Text('Can play native media during VR'),
          value: _canPlayNativeMedia,
          onChanged: (v) => setState(() => _canPlayNativeMedia = v),
        ),

        const SizedBox(height: 16),

        // --- Video ---
        _sectionHeader('Video (Channel 3)'),
        DropdownButtonFormField<String>(
          value: _resolutions.contains(_codecResolution)
              ? _codecResolution
              : _resolutions.first,
          decoration: const InputDecoration(labelText: 'Resolution'),
          items: _resolutions
              .map((v) => DropdownMenuItem(value: v, child: Text(v)))
              .toList(),
          onChanged: (v) => setState(() => _codecResolution = v!),
        ),
        const SizedBox(height: 8),
        DropdownButtonFormField<String>(
          value: _frameRates.contains(_frameRate)
              ? _frameRate
              : _frameRates.first,
          decoration: const InputDecoration(labelText: 'Frame Rate'),
          items: _frameRates
              .map((v) => DropdownMenuItem(value: v, child: Text(v)))
              .toList(),
          onChanged: (v) => setState(() => _frameRate = v!),
        ),
        const SizedBox(height: 8),
        TextField(
          controller: _densityCtrl,
          decoration: const InputDecoration(labelText: 'Density'),
          keyboardType: TextInputType.number,
        ),

        const SizedBox(height: 16),

        // --- Headunit Info ---
        _sectionHeader('Headunit Info'),
        TextField(
          controller: _huMakeCtrl,
          decoration: const InputDecoration(labelText: 'Make'),
        ),
        const SizedBox(height: 8),
        TextField(
          controller: _huModelCtrl,
          decoration: const InputDecoration(labelText: 'Model'),
        ),
        const SizedBox(height: 8),
        TextField(
          controller: _huYearCtrl,
          decoration: const InputDecoration(labelText: 'Year'),
        ),
        const SizedBox(height: 8),
        TextField(
          controller: _huSoftwareVersionCtrl,
          decoration: const InputDecoration(labelText: 'Software Version'),
        ),

        const SizedBox(height: 16),

        // --- Raw JSON preview ---
        _sectionHeader('Preview'),
        Container(
          padding: const EdgeInsets.all(8),
          decoration: BoxDecoration(
            color: Colors.grey[100],
            borderRadius: BorderRadius.circular(4),
          ),
          child: SelectableText(
            const JsonEncoder.withIndent('  ').convert(_buildConfig()),
            style: const TextStyle(fontFamily: 'monospace', fontSize: 11),
          ),
        ),

        const SizedBox(height: 80), // space for FAB
      ],
    );
  }

  Widget _sectionHeader(String title) {
    return Padding(
      padding: const EdgeInsets.only(bottom: 8),
      child: Text(
        title,
        style: Theme.of(context)
            .textTheme
            .titleMedium
            ?.copyWith(fontWeight: FontWeight.bold),
      ),
    );
  }
}

// ---------------------------------------------------------------------------
// Devices page
// ---------------------------------------------------------------------------

class DevicesPage extends StatefulWidget {
  const DevicesPage({super.key, required this.plugin});
  final OpenAutoBridge plugin;

  @override
  State<DevicesPage> createState() => _DevicesPageState();
}

class _DevicesPageState extends State<DevicesPage> {
  List<Map<String, dynamic>> _devices = [];
  StreamSubscription<Map<String, dynamic>>? _controlSub;
  bool _loading = true;

  @override
  void initState() {
    super.initState();
    _controlSub = widget.plugin.onControlReceived.listen(_onControl);
    widget.plugin.requestDevices();
  }

  @override
  void dispose() {
    _controlSub?.cancel();
    super.dispose();
  }

  void _onControl(Map<String, dynamic> msg) {
    if (msg['action'] == 'device_list') {
      final list = (msg['devices'] as List<dynamic>? ?? [])
          .cast<Map<String, dynamic>>();
      if (!mounted) return;
      setState(() {
        _devices = list;
        _loading = false;
      });
    }
  }

  Future<void> _connect(String id) async {
    await widget.plugin.connectDevice(id);
  }

  Future<void> _disconnect(String id) async {
    await widget.plugin.disconnectDevice(id);
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('Devices'),
        actions: [
          IconButton(
            icon: const Icon(Icons.refresh),
            tooltip: 'Refresh',
            onPressed: () {
              setState(() => _loading = true);
              widget.plugin.requestDevices();
            },
          ),
        ],
      ),
      body: _loading
          ? const Center(child: CircularProgressIndicator())
          : _devices.isEmpty
              ? const Center(child: Text('No devices available'))
              : ListView.builder(
                  itemCount: _devices.length,
                  itemBuilder: (context, index) {
                    final dev = _devices[index];
                    final id = dev['id'] as String? ?? '';
                    final type = dev['type'] as String? ?? 'unknown';
                    final name = dev['name'] as String? ?? id;
                    final status = dev['status'] as String? ?? 'available';
                    final isConnected = status == 'connected';
                    return ListTile(
                      leading: Icon(
                        type == 'wifi' ? Icons.wifi : Icons.usb,
                        color: isConnected ? Colors.green : null,
                      ),
                      title: Text(name),
                      subtitle: Text('$id • $status'),
                      trailing: isConnected
                          ? OutlinedButton(
                              onPressed: () => _disconnect(id),
                              child: const Text('Disconnect'),
                            )
                          : ElevatedButton(
                              onPressed: () => _connect(id),
                              child: const Text('Connect'),
                            ),
                    );
                  },
                ),
    );
  }
}
