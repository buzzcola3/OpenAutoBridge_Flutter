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
  final _openAutoBridgePlugin = OpenAutoBridge(
    config: OpenAutoConfig(videoHeightMargin: 150),
  );
  int? _videoTextureId;

  @override
  void initState() {
    super.initState();
    _initSensors();
    initPlatformState();
  }

  void _initSensors() {
    // Enable sensors + cyclic resend
    _openAutoBridgePlugin.config.sensors.nightMode
      ..enabled = true
      ..cyclic = true
      ..cyclicTime = 5000;
    _openAutoBridgePlugin.config.sensors.drivingStatus
      ..enabled = true
      ..cyclic = true
      ..cyclicTime = 5000;

    // Set initial sensor values
    _openAutoBridgePlugin.sensor.nightMode.set({'enabled': false});
    _openAutoBridgePlugin.sensor.drivingStatus.set({'status': 'no_video'});
  }

  @override
  void dispose() {
    _openAutoBridgePlugin.dispose();
    super.dispose();
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
                  icon: const Icon(Icons.volume_up),
                  tooltip: 'Audio',
                  onPressed: () {
                    Navigator.of(context).push(
                      MaterialPageRoute(
                        builder: (_) => AudioPage(plugin: _openAutoBridgePlugin),
                      ),
                    );
                  },
                ),
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
                      Flexible(
                        child: OpenAutoVideoView(
                          bridge: _openAutoBridgePlugin,
                          textureId: _videoTextureId!,
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
// Configuration page — edits config.* properties directly.
// Changes take effect on the next request_config handshake.
// ---------------------------------------------------------------------------

class ConfigPage extends StatefulWidget {
  const ConfigPage({super.key, required this.plugin});
  final OpenAutoBridge plugin;

  @override
  State<ConfigPage> createState() => _ConfigPageState();
}

class _ConfigPageState extends State<ConfigPage> {
  late TextEditingController _displayNameCtrl;
  late TextEditingController _densityCtrl;
  late TextEditingController _headunitMakeCtrl;
  late TextEditingController _headunitModelCtrl;
  late TextEditingController _headunitYearCtrl;
  late TextEditingController _headunitSoftwareVersionCtrl;

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

  OpenAutoConfig get _cfg => widget.plugin.config;

  @override
  void initState() {
    super.initState();
    _displayNameCtrl = TextEditingController(text: _cfg.displayName);
    _densityCtrl = TextEditingController(text: _cfg.videoDensity.toString());
    _headunitMakeCtrl = TextEditingController(text: _cfg.headunitMake);
    _headunitModelCtrl = TextEditingController(text: _cfg.headunitModel);
    _headunitYearCtrl = TextEditingController(text: _cfg.headunitYear);
    _headunitSoftwareVersionCtrl = TextEditingController(text: _cfg.headunitSoftwareVersion);
  }

  @override
  void dispose() {
    _displayNameCtrl.dispose();
    _densityCtrl.dispose();
    _headunitMakeCtrl.dispose();
    _headunitModelCtrl.dispose();
    _headunitYearCtrl.dispose();
    _headunitSoftwareVersionCtrl.dispose();
    super.dispose();
  }

  /// Writes text field values back to the config object.
  void _syncTextFields() {
    _cfg.displayName = _displayNameCtrl.text;
    _cfg.videoDensity = int.tryParse(_densityCtrl.text) ?? 140;
    _cfg.headunitMake = _headunitMakeCtrl.text;
    _cfg.headunitModel = _headunitModelCtrl.text;
    _cfg.headunitYear = _headunitYearCtrl.text;
    _cfg.headunitSoftwareVersion = _headunitSoftwareVersionCtrl.text;
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('Service Config')),
      body: _buildForm(),
    );
  }

  Widget _buildForm() {
    return ListView(
      padding: const EdgeInsets.all(16),
      children: [
        // --- General ---
        _sectionHeader('General'),
        TextField(
          controller: _displayNameCtrl,
          decoration: const InputDecoration(labelText: 'Display Name'),
          onChanged: (_) => _syncTextFields(),
        ),
        const SizedBox(height: 8),
        DropdownButtonFormField<String>(
          initialValue: _driverPositions.contains(_cfg.driverPosition)
              ? _cfg.driverPosition
              : _driverPositions.first,
          decoration: const InputDecoration(labelText: 'Driver Position'),
          items: _driverPositions
              .map((v) => DropdownMenuItem(value: v, child: Text(v)))
              .toList(),
          onChanged: (v) => setState(() => _cfg.driverPosition = v!),
        ),
        const SizedBox(height: 8),
        SwitchListTile(
          title: const Text('Can play native media during VR'),
          value: _cfg.canPlayNativeMediaDuringVr,
          onChanged: (v) => setState(() => _cfg.canPlayNativeMediaDuringVr = v),
        ),

        const SizedBox(height: 16),

        // --- Video ---
        _sectionHeader('Video (Channel 3)'),
        DropdownButtonFormField<String>(
          initialValue: _resolutions.contains(_cfg.videoCodecResolution)
              ? _cfg.videoCodecResolution
              : _resolutions.first,
          decoration: const InputDecoration(labelText: 'Resolution'),
          items: _resolutions
              .map((v) => DropdownMenuItem(value: v, child: Text(v)))
              .toList(),
          onChanged: (v) => setState(() => _cfg.videoCodecResolution = v!),
        ),
        const SizedBox(height: 8),
        DropdownButtonFormField<String>(
          initialValue: _frameRates.contains(_cfg.videoFrameRate)
              ? _cfg.videoFrameRate
              : _frameRates.first,
          decoration: const InputDecoration(labelText: 'Frame Rate'),
          items: _frameRates
              .map((v) => DropdownMenuItem(value: v, child: Text(v)))
              .toList(),
          onChanged: (v) => setState(() => _cfg.videoFrameRate = v!),
        ),
        const SizedBox(height: 8),
        TextField(
          controller: _densityCtrl,
          decoration: const InputDecoration(labelText: 'Density'),
          keyboardType: TextInputType.number,
          onChanged: (_) => _syncTextFields(),
        ),

        const SizedBox(height: 16),

        // --- Headunit Info ---
        _sectionHeader('Headunit Info'),
        TextField(
          controller: _headunitMakeCtrl,
          decoration: const InputDecoration(labelText: 'Make'),
          onChanged: (_) => _syncTextFields(),
        ),
        const SizedBox(height: 8),
        TextField(
          controller: _headunitModelCtrl,
          decoration: const InputDecoration(labelText: 'Model'),
          onChanged: (_) => _syncTextFields(),
        ),
        const SizedBox(height: 8),
        TextField(
          controller: _headunitYearCtrl,
          decoration: const InputDecoration(labelText: 'Year'),
          onChanged: (_) => _syncTextFields(),
        ),
        const SizedBox(height: 8),
        TextField(
          controller: _headunitSoftwareVersionCtrl,
          decoration: const InputDecoration(labelText: 'Software Version'),
          onChanged: (_) => _syncTextFields(),
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
            const JsonEncoder.withIndent('  ').convert(_cfg.buildServiceDiscovery()),
            style: const TextStyle(fontFamily: 'monospace', fontSize: 11),
          ),
        ),

        const SizedBox(height: 16),
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
// Audio page
// ---------------------------------------------------------------------------

class AudioPage extends StatefulWidget {
  const AudioPage({super.key, required this.plugin});
  final OpenAutoBridge plugin;

  @override
  State<AudioPage> createState() => _AudioPageState();
}

class _AudioPageState extends State<AudioPage> {
  late int _mediaVol;
  late int _guidanceVol;
  late int _systemVol;
  List<AudioDeviceInfo> _devices = [];
  String _selectedDevice = '';
  bool _loadingDevices = true;

  @override
  void initState() {
    super.initState();
    _mediaVol = widget.plugin.audio.media.volume;
    _guidanceVol = widget.plugin.audio.guidance.volume;
    _systemVol = widget.plugin.audio.system.volume;
    _selectedDevice = widget.plugin.config.audio.device;
    _loadDevices();
  }

  Future<void> _loadDevices() async {
    final devices = await widget.plugin.audio.devices;
    if (!mounted) return;
    setState(() {
      _devices = devices;
      _loadingDevices = false;
    });
  }

  void _setVolume(AudioChannelHandle channel, int value) {
    channel.volume = value;
    setState(() {
      _mediaVol = widget.plugin.audio.media.volume;
      _guidanceVol = widget.plugin.audio.guidance.volume;
      _systemVol = widget.plugin.audio.system.volume;
    });
  }

  void _setDevice(String device) {
    widget.plugin.config.audio.device = device;
    setState(() => _selectedDevice = device);
  }

  Widget _volumeSlider(String label, int value, AudioChannelHandle channel) {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Text('$label: $value%'),
        Slider(
          value: value.toDouble(),
          min: 0,
          max: 100,
          divisions: 20,
          label: '$value',
          onChanged: (v) => _setVolume(channel, v.round()),
        ),
      ],
    );
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('Audio'),
        actions: [
          IconButton(
            icon: const Icon(Icons.refresh),
            tooltip: 'Refresh devices',
            onPressed: () {
              setState(() => _loadingDevices = true);
              _loadDevices();
            },
          ),
        ],
      ),
      body: ListView(
        padding: const EdgeInsets.all(16),
        children: [
          Text('Volume',
              style: Theme.of(context)
                  .textTheme
                  .titleMedium
                  ?.copyWith(fontWeight: FontWeight.bold)),
          const SizedBox(height: 8),
          _volumeSlider('Media', _mediaVol, widget.plugin.audio.media),
          _volumeSlider('Guidance', _guidanceVol, widget.plugin.audio.guidance),
          _volumeSlider('System', _systemVol, widget.plugin.audio.system),
          const Divider(height: 32),
          Text('Output Device',
              style: Theme.of(context)
                  .textTheme
                  .titleMedium
                  ?.copyWith(fontWeight: FontWeight.bold)),
          const SizedBox(height: 8),
          if (_loadingDevices)
            const Center(child: CircularProgressIndicator())
          else if (_devices.isEmpty)
            const Text('No audio devices found')
          else
            DropdownButtonFormField<String>(
              initialValue: _devices.any((d) => d.name == _selectedDevice)
                  ? _selectedDevice
                  : '',
              decoration:
                  const InputDecoration(labelText: 'Audio output'),
              items: [
                const DropdownMenuItem(
                  value: '',
                  child: Text('Default'),
                ),
                ..._devices.map((d) => DropdownMenuItem(
                      value: d.name,
                      child: Text(d.displayName),
                    )),
              ],
              onChanged: (v) => _setDevice(v ?? ''),
            ),
        ],
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
