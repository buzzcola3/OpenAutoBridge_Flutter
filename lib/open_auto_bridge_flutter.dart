
import 'dart:async';
import 'dart:convert';

import 'open_auto_bridge_flutter_platform_interface.dart';

/// Touch actions mirrored on native side.
enum TouchAction {
  down,
  up,
  moved,
  pointerDown,
  pointerUp,
}

extension TouchActionCode on TouchAction {
  int get code {
    switch (this) {
      case TouchAction.down:
        return 0;
      case TouchAction.up:
        return 1;
      case TouchAction.moved:
        return 2;
      case TouchAction.pointerDown:
        return 3;
      case TouchAction.pointerUp:
        return 4;
    }
  }
}

/// All sensor types supported by OpenAutoCore.
///
/// Each value maps to a JSON key for sensor data and a proto enum string
/// for the service discovery config.
enum SensorType {
  location,
  compass,
  speed,
  rpm,
  odometer,
  fuel,
  parkingBrake,
  gear,
  nightMode,
  environment,
  hvac,
  drivingStatus,
  deadReckoning,
  passenger,
  door,
  light,
  tirePressure,
  accelerometer,
  gyroscope,
  gpsSatellite,
  tollCard,
}

extension SensorTypeStrings on SensorType {
  /// The JSON key used in sensor data payloads.
  String get jsonKey {
    switch (this) {
      case SensorType.location:        return 'location';
      case SensorType.compass:         return 'compass';
      case SensorType.speed:           return 'speed';
      case SensorType.rpm:             return 'rpm';
      case SensorType.odometer:        return 'odometer';
      case SensorType.fuel:            return 'fuel';
      case SensorType.parkingBrake:    return 'parking_brake';
      case SensorType.gear:            return 'gear';
      case SensorType.nightMode:       return 'night_mode';
      case SensorType.environment:     return 'environment';
      case SensorType.hvac:            return 'hvac';
      case SensorType.drivingStatus:   return 'driving_status';
      case SensorType.deadReckoning:   return 'dead_reckoning';
      case SensorType.passenger:       return 'passenger';
      case SensorType.door:            return 'door';
      case SensorType.light:           return 'light';
      case SensorType.tirePressure:    return 'tire_pressure';
      case SensorType.accelerometer:   return 'accelerometer';
      case SensorType.gyroscope:       return 'gyroscope';
      case SensorType.gpsSatellite:    return 'gps_satellite';
      case SensorType.tollCard:        return 'toll_card';
    }
  }

  /// The proto enum string used in the service discovery config.
  String get protoEnum {
    switch (this) {
      case SensorType.location:        return 'SENSOR_LOCATION';
      case SensorType.compass:         return 'SENSOR_COMPASS';
      case SensorType.speed:           return 'SENSOR_SPEED';
      case SensorType.rpm:             return 'SENSOR_RPM';
      case SensorType.odometer:        return 'SENSOR_ODOMETER';
      case SensorType.fuel:            return 'SENSOR_FUEL';
      case SensorType.parkingBrake:    return 'SENSOR_PARKING_BRAKE';
      case SensorType.gear:            return 'SENSOR_GEAR';
      case SensorType.nightMode:       return 'SENSOR_NIGHT_MODE';
      case SensorType.environment:     return 'SENSOR_ENVIRONMENT_DATA';
      case SensorType.hvac:            return 'SENSOR_HVAC_DATA';
      case SensorType.drivingStatus:   return 'SENSOR_DRIVING_STATUS_DATA';
      case SensorType.deadReckoning:   return 'SENSOR_DEAD_RECKONING_DATA';
      case SensorType.passenger:       return 'SENSOR_PASSENGER_DATA';
      case SensorType.door:            return 'SENSOR_DOOR_DATA';
      case SensorType.light:           return 'SENSOR_LIGHT_DATA';
      case SensorType.tirePressure:    return 'SENSOR_TIRE_PRESSURE_DATA';
      case SensorType.accelerometer:   return 'SENSOR_ACCELEROMETER_DATA';
      case SensorType.gyroscope:       return 'SENSOR_GYROSCOPE_DATA';
      case SensorType.gpsSatellite:    return 'SENSOR_GPS_SATELLITE_DATA';
      case SensorType.tollCard:        return 'SENSOR_TOLL_CARD';
    }
  }

  /// Look up a [SensorType] by its proto enum string.
  static SensorType? fromProtoEnum(String value) {
    for (final t in SensorType.values) {
      if (t.protoEnum == value) return t;
    }
    return null;
  }
}

// ── Internal per-sensor state ─────────────────────────────────────────

class _SensorState {
  final SensorType type;

  bool enabled = false;
  bool cyclic = false;
  int cyclicTime = 5000;
  Map<String, dynamic>? data;
  Timer? _timer;

  _SensorState(this.type);

  void send() {
    final d = data;
    if (d == null || !enabled) return;
    OpenAutoBridgePlatform.instance.sendSensorJson(
      jsonEncode({type.jsonKey: d}),
    );
  }

  void reconcileTimer() {
    _timer?.cancel();
    _timer = null;
    if (enabled && cyclic && data != null) {
      _timer = Timer.periodic(
        Duration(milliseconds: cyclicTime),
        (_) => send(),
      );
    }
  }

  void dispose() {
    _timer?.cancel();
    _timer = null;
  }
}

// ── Sensor config (per-sensor settings) ──────────────────────────────

/// Configuration for a single sensor: whether it is advertised to the
/// phone, whether its value is re-sent cyclically, and the cycle interval.
///
/// Property changes take effect locally immediately (timers, sending).
/// Call [OpenAutoConfig.apply] to push the enabled-sensor list to the core.
class SensorConfig {
  final _SensorState _s;
  SensorConfig._(this._s);

  bool get enabled => _s.enabled;
  set enabled(bool v) {
    _s.enabled = v;
    _s.reconcileTimer();
  }

  bool get cyclic => _s.cyclic;
  set cyclic(bool v) {
    _s.cyclic = v;
    _s.reconcileTimer();
  }

  int get cyclicTime => _s.cyclicTime;
  set cyclicTime(int v) {
    _s.cyclicTime = v;
    _s.reconcileTimer();
  }
}

// ── Sensors config collection ────────────────────────────────────────

/// Named access to [SensorConfig] entries for every sensor type.
class SensorsConfig {
  final Map<SensorType, _SensorState> _states;
  SensorsConfig._(this._states);

  SensorConfig operator [](SensorType type) =>
      SensorConfig._(_states[type]!);

  SensorConfig get location      => this[SensorType.location];
  SensorConfig get compass       => this[SensorType.compass];
  SensorConfig get speed         => this[SensorType.speed];
  SensorConfig get rpm           => this[SensorType.rpm];
  SensorConfig get odometer      => this[SensorType.odometer];
  SensorConfig get fuel          => this[SensorType.fuel];
  SensorConfig get parkingBrake  => this[SensorType.parkingBrake];
  SensorConfig get gear          => this[SensorType.gear];
  SensorConfig get nightMode     => this[SensorType.nightMode];
  SensorConfig get environment   => this[SensorType.environment];
  SensorConfig get hvac          => this[SensorType.hvac];
  SensorConfig get drivingStatus => this[SensorType.drivingStatus];
  SensorConfig get deadReckoning => this[SensorType.deadReckoning];
  SensorConfig get passenger     => this[SensorType.passenger];
  SensorConfig get door          => this[SensorType.door];
  SensorConfig get light         => this[SensorType.light];
  SensorConfig get tirePressure  => this[SensorType.tirePressure];
  SensorConfig get accelerometer => this[SensorType.accelerometer];
  SensorConfig get gyroscope     => this[SensorType.gyroscope];
  SensorConfig get gpsSatellite  => this[SensorType.gpsSatellite];
  SensorConfig get tollCard      => this[SensorType.tollCard];
}

// ── Audio ────────────────────────────────────────────────────────────

/// Audio channel identifiers matching the native C++ side.
enum AudioChannel { media, guidance, system }

/// Information about an available audio output device.
class AudioDeviceInfo {
  final String name;
  final String displayName;
  const AudioDeviceInfo({required this.name, required this.displayName});

  @override
  String toString() => 'AudioDeviceInfo(name: $name, displayName: $displayName)';
}

/// Per-channel volume handle. Lives on [AudioManager], not config.
class AudioChannelHandle {
  final AudioChannel channel;
  int _volume = 100;

  AudioChannelHandle._(this.channel);

  int get volume => _volume;
  set volume(int v) {
    _volume = v.clamp(0, 100);
    OpenAutoBridgePlatform.instance.setAudioVolume(channel.name, _volume);
  }
}

/// Runtime audio controls: per-channel volume + device enumeration.
///
/// Access via `bridge.audio`.
class AudioManager {
  final AudioChannelHandle media    = AudioChannelHandle._(AudioChannel.media);
  final AudioChannelHandle guidance = AudioChannelHandle._(AudioChannel.guidance);
  final AudioChannelHandle system   = AudioChannelHandle._(AudioChannel.system);

  /// Queries the native side for available audio output devices.
  Future<List<AudioDeviceInfo>> get devices async {
    final raw = await OpenAutoBridgePlatform.instance.getAudioDevices();
    return raw
        .map((m) => AudioDeviceInfo(
              name: m['name'] ?? '',
              displayName: m['display_name'] ?? m['name'] ?? '',
            ))
        .toList();
  }
}

/// Audio device selection. Lives on [OpenAutoConfig].
class AudioConfig {
  String _device = '';
  String get device => _device;
  set device(String v) {
    _device = v;
    OpenAutoBridgePlatform.instance.setAudioDevice(v);
  }
}

// ── OpenAutoConfig ───────────────────────────────────────────────────

/// Top-level configuration object.
///
/// Modify [sensors] properties, then call [apply] to push the updated
/// service-discovery response to OpenAutoCore.
class OpenAutoConfig {
  final SensorsConfig sensors;
  final AudioConfig audio = AudioConfig();
  final OpenAutoBridge _bridge;

  OpenAutoConfig._(Map<SensorType, _SensorState> states, this._bridge)
      : sensors = SensorsConfig._(states);

  /// Pushes the current sensor configuration to the core.
  ///
  /// Fetches the running service-discovery config, rebuilds the
  /// `sensor_source_service.sensors` list from the enabled flags,
  /// and sends it back.
  Future<void> apply() async {
    final config = await _bridge.getConfig();
    if (config == null) return;

    final channels = config['channels'] as List<dynamic>?;
    if (channels == null) return;

    final sensorList = sensors._states.entries
        .where((e) => e.value.enabled)
        .map((e) => {'sensor_type': e.key.protoEnum})
        .toList();

    bool found = false;
    for (final ch in channels) {
      if (ch is Map<String, dynamic> && ch['id'] == 1) {
        ch['sensor_source_service'] = {'sensors': sensorList};
        found = true;
        break;
      }
    }
    if (!found) {
      channels.add({
        'id': 1,
        'sensor_source_service': {'sensors': sensorList},
      });
    }

    await _bridge.sendConfigSet(config);
  }
}

// ── Sensor handle (per-sensor data setter) ───────────────────────────

/// Handle for pushing data to a single sensor.
///
/// [set] sends the value immediately (if the sensor is enabled) and,
/// when cyclic mode is on, starts periodic re-sending at the configured
/// interval.
class SensorHandle {
  final _SensorState _s;
  SensorHandle._(this._s);

  void set(Map<String, dynamic> data) {
    _s.data = data;
    _s.send();
    _s.reconcileTimer();
  }
}

// ── Sensor manager ───────────────────────────────────────────────────

/// Named access to [SensorHandle] entries for every sensor type.
class SensorManager {
  final Map<SensorType, _SensorState> _states;
  SensorManager._(this._states);

  SensorHandle operator [](SensorType type) =>
      SensorHandle._(_states[type]!);

  SensorHandle get location      => this[SensorType.location];
  SensorHandle get compass       => this[SensorType.compass];
  SensorHandle get speed         => this[SensorType.speed];
  SensorHandle get rpm           => this[SensorType.rpm];
  SensorHandle get odometer      => this[SensorType.odometer];
  SensorHandle get fuel          => this[SensorType.fuel];
  SensorHandle get parkingBrake  => this[SensorType.parkingBrake];
  SensorHandle get gear          => this[SensorType.gear];
  SensorHandle get nightMode     => this[SensorType.nightMode];
  SensorHandle get environment   => this[SensorType.environment];
  SensorHandle get hvac          => this[SensorType.hvac];
  SensorHandle get drivingStatus => this[SensorType.drivingStatus];
  SensorHandle get deadReckoning => this[SensorType.deadReckoning];
  SensorHandle get passenger     => this[SensorType.passenger];
  SensorHandle get door          => this[SensorType.door];
  SensorHandle get light         => this[SensorType.light];
  SensorHandle get tirePressure  => this[SensorType.tirePressure];
  SensorHandle get accelerometer => this[SensorType.accelerometer];
  SensorHandle get gyroscope     => this[SensorType.gyroscope];
  SensorHandle get gpsSatellite  => this[SensorType.gpsSatellite];
  SensorHandle get tollCard      => this[SensorType.tollCard];
}

// ── Main bridge ──────────────────────────────────────────────────────

class OpenAutoBridge {
  late final Map<SensorType, _SensorState> _sensorStates;

  /// Sensor configuration (enabled / cyclic / cyclicTime per sensor).
  /// Call [OpenAutoConfig.apply] after changing to sync with the core.
  late final OpenAutoConfig config;

  /// Sensor data handles. Use `sensor.<name>.set(data)` to push values.
  late final SensorManager sensor;

  /// Audio controls: per-channel volume and device enumeration.
  final AudioManager audio = AudioManager();

  OpenAutoBridge() {
    _sensorStates = {
      for (final type in SensorType.values) type: _SensorState(type),
    };
    config = OpenAutoConfig._(_sensorStates, this);
    sensor = SensorManager._(_sensorStates);
  }

  /// Cancels all periodic sensor timers. Call when done.
  void dispose() {
    for (final state in _sensorStates.values) {
      state.dispose();
    }
  }
  Future<String?> getPlatformVersion() {
    return OpenAutoBridgePlatform.instance.getPlatformVersion();
  }

  Future<int?> getVideoTextureId() {
    return OpenAutoBridgePlatform.instance.getVideoTextureId();
  }

  Future<void> sendTouchEvent({
    required int pointerId,
    required double x,
    required double y,
    required TouchAction action,
  }) {
    return OpenAutoBridgePlatform.instance.sendTouchEvent(
      pointerId: pointerId,
      x: x,
      y: y,
      actionCode: action.code,
    );
  }

  /// Sends a JSON string for one of the supported sensor payloads:
  ///
  /// Location:
  /// {
  ///   "location": {
  ///     "latitude": 37.7749,
  ///     "longitude": -122.4194,
  ///     "accuracy_m": 5.0,
  ///     "altitude_m": 15.0,
  ///     "speed_mps": 13.4,
  ///     "bearing_deg": 90.0
  ///   }
  /// }
  /// Required: latitude, longitude
  /// Optional: accuracy_m, altitude_m, speed_mps, bearing_deg
  ///
  /// Night mode:
  /// { "night_mode": { "enabled": true } }
  /// Required: enabled (boolean)
  ///
  /// Driving status:
  /// { "driving_status": { "status": "no_video" } }
  /// Required: status (string)
  /// Allowed values: unrestricted, no_video, no_keyboard_input,
  ///   no_voice_input, no_config, limit_message_len
  Future<void> sendSensorJson(String json) {
    return OpenAutoBridgePlatform.instance.sendSensorJson(json);
  }

  /// Requests the current config from the core (side A) over transport.
  ///
  /// Sends a `{"action": "get"}` message over `MsgType::CONFIGURATION`
  /// and waits for the core to respond with the current config JSON.
  /// If the core does not respond within 5 seconds, returns `null`.
  ///
  /// Returns the parsed JSON map, or `null` on timeout / error.
  Future<Map<String, dynamic>?> getConfig() {
    return OpenAutoBridgePlatform.instance.getConfig();
  }

  /// Replaces the running config on the core (side A).
  ///
  /// [config] must match the schema of
  /// `ServiceDiscoveryResponse.default.json`. The core validates the
  /// JSON by constructing the protobuf; if validation fails the request
  /// is silently dropped.
  ///
  /// Fire-and-forget — there is no response.
  Future<void> sendConfigSet(Map<String, dynamic> config) {
    final request = jsonEncode({'action': 'set', 'config': config});
    return OpenAutoBridgePlatform.instance.sendConfigJson(request);
  }

  /// Deletes the user overlay and reloads the shipped default on the
  /// core (side A).
  ///
  /// Fire-and-forget — there is no response.
  Future<void> sendConfigReset() {
    final request = jsonEncode({'action': 'reset'});
    return OpenAutoBridgePlatform.instance.sendConfigJson(request);
  }

  // ── Device control ──────────────────────────────────────────────────

  /// Stream of control messages from the core (e.g. device_list updates).
  Stream<Map<String, dynamic>> get onControlReceived {
    return OpenAutoBridgePlatform.instance.onControlReceived;
  }

  /// Requests the current list of available devices from the core.
  ///
  /// The core will respond asynchronously via [onControlReceived] with
  /// `{"action": "device_list", "devices": [...]}`.
  Future<void> requestDevices() {
    final request = jsonEncode({'action': 'get_devices'});
    return OpenAutoBridgePlatform.instance.sendControlJson(request);
  }

  /// Tells the core to connect to the device with the given [id].
  ///
  /// [id] is the device identifier from the device list (e.g. "usb:18d1:2d01").
  /// The core sets the device status to "connected" and broadcasts
  /// an updated device_list.
  Future<void> connectDevice(String id) {
    final request = jsonEncode({'action': 'connect_device', 'id': id});
    return OpenAutoBridgePlatform.instance.sendControlJson(request);
  }

  /// Tells the core to disconnect the device with the given [id].
  ///
  /// The core stops the AA session and marks the device back as
  /// "available", then broadcasts an updated device_list.
  Future<void> disconnectDevice(String id) {
    final request = jsonEncode({'action': 'disconnect_device', 'id': id});
    return OpenAutoBridgePlatform.instance.sendControlJson(request);
  }
}
