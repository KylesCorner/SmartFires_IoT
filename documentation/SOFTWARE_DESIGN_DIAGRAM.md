# SmartFires IoT Software Design Diagram

## System Diagram

```mermaid
%%{init: {"flowchart": {"defaultRenderer": "elk"}} }%%
flowchart LR
    subgraph Node[Feather M0 Node]
        Sensors[Sensors\nSHT31 GPS Wind SPS30 IMU]
        Duty[DutyCycleController]
        App[SmartFiresNodeApp]
        Snapshot[SensorSnapshot]
        Handler[PacketHandler]
        Clock[TdmaClock]
        Queue[TdmaTxQueue]
        RadioSvc[TdmaRadioService]
        DriverNode[RadioHeadTdmaDriver]
        Battery[BatteryMonitor]

        Sensors --> Snapshot
        Battery --> Snapshot
        Duty --> App
        App --> Snapshot
        Snapshot --> Handler
        Clock --> App
        Clock --> RadioSvc
        Handler --> Queue
        Queue --> RadioSvc
        RadioSvc --> DriverNode
        App --> RadioSvc
    end

    subgraph Air[LoRa 915 MHz]
        LoRa[(TDMA Channel)]
    end

    subgraph Base[Feather M0 Base Station]
        DriverBase[RadioHeadTdmaDriver]
        BaseApp[SmartFiresBaseApp]
        UartBridge[UART Framing and Parsing]

        DriverBase --> BaseApp
        BaseApp --> UartBridge
        UartBridge --> BaseApp
    end

    subgraph Jetson[Jetson Orin Nano]
        Ingest[ingest_service.py]
        Decode[packet.py decode and bundle expansion]
        Csv[CSV logging]
        Sync[TIME_SYNC generation]
        Anemometer[Optional anemometer polling]
        Web[web/app.py FastAPI dashboard\nmap, telemetry charts, command POST]
        Sniffer[sniffer_service.py\noptional passive sniffer Feather]

        Ingest --> Decode
        Decode --> Csv
        Sync --> Ingest
        Anemometer --> Csv
        Ingest --> Web
        Sniffer --> Web
    end

    DriverNode --> LoRa
    LoRa --> DriverBase
    UartBridge --> Ingest
    Ingest --> UartBridge
```

## Node Control Flow

```mermaid
%%{init: {"flowchart": {"defaultRenderer": "elk"}} }%%
flowchart TD
    Start[boot] --> Begin[SmartFiresNodeApp.begin]
    Begin --> Init[initialize battery duty radio]
    Init --> Awaken[enqueue AWAKEN packet]
    Awaken --> Loop[main loop]

    Loop --> RadioUpdate[radio.update]
    RadioUpdate --> DutyUpdate[duty.update]
    DutyUpdate --> Ready{telemetry ready}
    Ready -- no --> Loop
    Ready -- yes --> Build[build SensorSnapshot]
    Build --> Push[PacketHandler.push]
    Push --> Status{status packet ready}
    Status -- yes --> EnqueueStatus[enqueue STATUS]
    Status -- no --> Bundle
    EnqueueStatus --> Bundle{bundle ready}
    Bundle -- yes --> EnqueueBundle[enqueue BUNDLE]
    Bundle -- no --> MarkSent[mark telemetry sent]
    EnqueueBundle --> MarkSent
    MarkSent --> Loop
```

## Base Station Control Flow

```mermaid
%%{init: {"flowchart": {"defaultRenderer": "elk"}} }%%
flowchart TD
    BaseLoop[SmartFiresBaseApp.update] --> LoRaRx[processIncomingLoRa]
    LoRaRx --> Forward[encode base UART frame and forward to Jetson]
    Forward --> JetsonRx[processIncomingJetsonUart]
    JetsonRx --> Parse[validate UART frame and CRC]
    Parse --> Route{packet type}
    Route -- TIME_SYNC --> Broadcast[broadcast TIME_SYNC over LoRa]
    Route -- ACK_SUMMARY --> Targeted[target send to node]
    Route -- CMD_CALIBRATE / CMD_RESET --> Cmd[target send command to node]
    Route -- CMD_ACK from node --> RelayAck[relay ACK to Jetson over UART]
    Route -- other --> Ignore[ignore]
    Broadcast --> Health[maybeLogHealth]
    Targeted --> Health
    Cmd --> Health
    RelayAck --> Health
    Ignore --> Health
```

## Node Hardware Diagram

The diagram below is a clean reconstruction of the current node wiring draft from the whiteboard. It should be treated as an implementation guide, not as a PCB-ready schematic. The wind-sensor power switching block in particular should be checked against the final breadboard before fabrication.

```mermaid
%%{init: {"flowchart": {"defaultRenderer": "elk"}} }%%
flowchart TB
    Battery[Battery]
    Feather[Feather M0 RFM95 LoRa Board\nUART: TX/RX\nI2C: SDA/SCL\nPins: D11 D12 A1 A2\nProvides +5 V and +3.3 V rails]
    Rail5[(+5 V Rail)]

    WindCtrl[D11 via 4.7 k resistor]
    WindSwitch[Wind power-switch stage\n2N3906 + 2N3904 + 10 k bias]
    subgraph DividerPair[ ]
        direction LR
        DivRV[RV divider\n15 k / 22 k]
        DivTMP[TMP divider\n15 k / 22 k]
    end

    SPS30[SPS30 PM Sensor]
    Wind[Wind Sensor Rev C]

    subgraph Sensors[I2C Sensors]
        direction LR
        GPS[PA1010D GPS]
        IMU[ICM-20948 IMU]
        SHT31[SHT31 Temp/Humidity]
    end

    GSPS30((GR))
    GGPS((GR))
    GIMU((GR))
    GSHT31((GR))
    GWind((GR))

    Battery --> Feather
    Feather --> Rail5

    Feather -- TX to SPS30 RX --> SPS30
    SPS30 -- TX to Feather RX --> Feather
    Rail5 --> SPS30
    SPS30 --- GSPS30

    Feather -- SDA/SCL --> GPS
    GPS -- SDA/SCL --> IMU
    IMU -- SDA/SCL --> SHT31
    Feather -- +3.3 V --> GPS
    Feather -- +3.3 V --> IMU
    Feather -- +3.3 V --> SHT31
    GPS --- GGPS
    IMU --- GIMU
    SHT31 --- GSHT31

    Feather -- D12 wake --> GPS

    IMU -. AD0 to GND .-> GIMU
    SHT31 -- ADDR and RST tied high --> Feather

    Feather --> WindCtrl
    WindCtrl --> WindSwitch
    Rail5 --> WindSwitch
    WindSwitch --> Wind
    Wind --- GWind

    Wind --> DivRV
    Wind --> DivTMP
    DivRV -- A1 --> Feather
    DivTMP -- A2 --> Feather
```

## Node Wiring Notes

- `Battery` feeds the Feather LoRa board directly.
- Ground is shown as short per-device tails rather than as a shared ground rail, to match the whiteboard sketch more closely.
- The Feather LoRa board is shown as the source of the distributed `+5 V` and `+3.3 V` connections.
- `GPS`, `IMU`, and `SHT31` share the Feather I2C connection directly, shown as a loop across the sensor row rather than as a separate bus block, and appear to run from `+3.3 V`.
- `GPS wake` is drawn to Feather `D12`.
- `SPS30` is drawn on UART with crossed TX/RX and `VIN` tied to `+5 V`; the whiteboard also calls out a logic-level question that should be verified during bring-up.
- `IMU AD0` is shown tied to ground.
- `SHT31 ADDR` and `RST` are shown tied high to `+3.3 V`.
- The wind sensor block is drawn with a transistor-switched supply driven from `D11` through a `4.7 k` resistor, using `2N3906` and `2N3904` transistors plus a `10 k` bias resistor.
- Wind sensor outputs labeled `RV` and `TMP` are drawn into Feather analog inputs `A1` and `A2` through resistor-divider networks labeled `15 k` and `22 k`.
- Final resistor values and transistor orientation should be checked against the actual node prototype before treating this as authoritative hardware documentation.
