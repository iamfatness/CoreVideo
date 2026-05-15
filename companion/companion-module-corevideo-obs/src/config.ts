import type { SomeCompanionConfigField } from '@companion-module/base'

export interface CoreVideoConfig {
	// CoreVideo OBS plugin
	pluginHost: string
	pluginPort: number
	pluginToken: string
	// OBS WebSocket (native)
	obsEnabled: boolean
	obsHost: string
	obsPort: number
	obsPassword: string
	// CoreVideo Sidecar
	sidecarEnabled: boolean
	sidecarHost: string
	sidecarPort: number
}

export const configFields: SomeCompanionConfigField[] = [
	// ── CoreVideo Plugin ──────────────────────────────────────────────────────
	{
		type: 'static-text', id: 'h1', label: 'CoreVideo Plugin (Zoom Control)',
		value: 'Connects to the CoreVideo OBS plugin\'s TCP control server.',
		width: 12,
	},
	{
		type: 'textinput', id: 'pluginHost', label: 'Plugin Host',
		default: '127.0.0.1', width: 8,
	},
	{
		type: 'number', id: 'pluginPort', label: 'Port',
		default: 19870, min: 1, max: 65535, width: 4,
	},
	{
		type: 'textinput', id: 'pluginToken', label: 'Auth Token (leave blank if none)',
		default: '', width: 12,
	},

	// ── OBS WebSocket ─────────────────────────────────────────────────────────
	{
		type: 'static-text', id: 'h2', label: 'OBS Studio (WebSocket)',
		value: 'Connects directly to OBS via obs-websocket v5 (built into OBS 28+).',
		width: 12,
	},
	{
		type: 'checkbox', id: 'obsEnabled', label: 'Enable OBS WebSocket connection',
		default: true, width: 12,
	},
	{
		type: 'textinput', id: 'obsHost', label: 'OBS Host',
		default: '127.0.0.1', width: 8,
	},
	{
		type: 'number', id: 'obsPort', label: 'Port',
		default: 4455, min: 1, max: 65535, width: 4,
	},
	{
		type: 'textinput', id: 'obsPassword', label: 'OBS WebSocket Password',
		default: '', width: 12,
	},

	// ── CoreVideo Sidecar ─────────────────────────────────────────────────────
	{
		type: 'static-text', id: 'h3', label: 'CoreVideo Sidecar (Show Control)',
		value: 'Connects to the CoreVideo Sidecar app for show phase and layout control.',
		width: 12,
	},
	{
		type: 'checkbox', id: 'sidecarEnabled', label: 'Enable Sidecar connection',
		default: false, width: 12,
	},
	{
		type: 'textinput', id: 'sidecarHost', label: 'Sidecar Host',
		default: '127.0.0.1', width: 8,
	},
	{
		type: 'number', id: 'sidecarPort', label: 'Port',
		default: 19880, min: 1, max: 65535, width: 4,
	},
]
