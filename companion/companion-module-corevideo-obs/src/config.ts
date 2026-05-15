import type { SomeCompanionConfigField } from '@companion-module/base'

export interface CoreVideoConfig {
	host: string
	port: number
	token: string
}

export const configFields: SomeCompanionConfigField[] = [
	{
		type: 'static-text',
		id: 'info',
		label: '',
		value:
			'Connects to the CoreVideo control server running on the same machine as OBS Studio. ' +
			'Enable remote access in CoreVideo → Settings → Control Server if OBS is on a different host.',
		width: 12,
	},
	{
		type: 'textinput',
		id: 'host',
		label: 'OBS Host',
		default: '127.0.0.1',
		width: 8,
	},
	{
		type: 'number',
		id: 'port',
		label: 'TCP Port',
		default: 19870,
		min: 1,
		max: 65535,
		width: 4,
	},
	{
		type: 'textinput',
		id: 'token',
		label: 'Auth Token (leave blank if none set)',
		default: '',
		width: 12,
	},
]
