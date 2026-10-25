import * as m from 'zigbee-herdsman-converters/lib/modernExtend';
import * as exposes from 'zigbee-herdsman-converters/lib/exposes';
import * as ota from 'zigbee-herdsman-converters/lib/ota';

const ea = exposes.access;

const fzLocal = {
    radar_distance: {
        cluster: 'genAnalogInput',
        type: ['attributeReport', 'readResponse'],
        convert: (model, msg, publish, options, meta) => {
            if (msg.endpoint.ID === 14 && msg.data.presentValue !== undefined) {
                return { distance: Math.round(msg.data.presentValue) };
            }
        },
    },
};

export default {
    zigbeeModel: ['MultiSensor-ZR'],
    model: 'MultiSensor-ZR',
    vendor: 'SmartSense Lab',
    description: 'SmartSense Lab - Multisensore ESP32-C6 Router (LD2410C, SHT40, BH1750)',
    icon: 'https://raw.githubusercontent.com/R0b3rt0M/smartsense/main/docs/assets/MultiSensor-ZR.png',
    extend: [
        m.occupancy(),
        m.temperature({
            reporting: {min: 10, max: 300, change: 10},
        }),
        m.humidity({
            reporting: {min: 10, max: 300, change: 50},
        }),
        m.illuminance({
            reporting: {min: 10, max: 300, change: 5},
        }),
    ],
    fromZigbee: [
        fzLocal.radar_distance,
    ],
    toZigbee: [],
    exposes: [
        exposes.numeric('distance', ea.STATE)
            .withUnit('cm')
            .withDescription('Distanza bersaglio LD2410C'),
    ],
    ota: ota.zigbeeOTA,
    meta: {
        multiEndpoint: true,
    },
    endpoint: (device) => {
        return {
            occupancy: 10,
            temperature: 11,
            humidity: 11,
            illuminance: 12,
            distance: 14,
        };
    },
};