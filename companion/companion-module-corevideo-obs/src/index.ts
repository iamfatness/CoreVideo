import {
	InstanceBase,
	InstanceStatus,
	runEntrypoint,
	type SomeCompanionConfigField,
} from '@companion-module/base'
import * as net from 'net'
import { configFields, type CoreVideoConfig } from './config.js'
import { defaultState, type ModuleState } from './state.js'
import { buildActions } from './actions.js'
import { buildFeedbacks } from './feedbacks.js'
import { variableDefinitions, buildVariableValues, buildOutputVariableDefs } from './variables.js'
import { OBSWebSocketClient } from './obs-ws-client.js'
import { SidecarClient } from './sidecar-client.js'

export class CoreVideoInstance extends InstanceBase<CoreVideoConfig> {
	public state: ModuleState = defaultState()

	// CoreVideo plugin TCP
	private pluginSocket: net.Socket | null = null
	private pluginBuffer = ''
	private pluginReconnect: ReturnType<typeof setTimeout> | null = null

	// OBS WebSocket
	private obsClient: OBSWebSocketClient | null = null

	// Sidecar TCP
	private sidecarClient: SidecarClient | null = null

	private destroyed = false

	// ── Lifecycle ──────────────────────────────────────────────────────────────

	async init(config: CoreVideoConfig): Promise<void> {
		this.config = config
		this.state = defaultState()
		this.setVariableDefinitions([...variableDefinitions, ...buildOutputVariableDefs(8)])
		this.setVariableValues(buildVariableValues(this.state))
		this.setActionDefinitions(buildActions(this))
		this.setFeedbackDefinitions(buildFeedbacks(this))
		this.updateStatus(InstanceStatus.Disconnected)
		this.connectPlugin()
		if (config.obsEnabled !== false) this.connectOBS()
		if (config.sidecarEnabled) this.connectSidecar()
	}

	async destroy(): Promise<void> {
		this.destroyed = true
		this.clearPluginReconnect()
		this.pluginSocket?.destroy()
		this.pluginSocket = null
		this.obsClient?.destroy()
		this.obsClient = null
		this.sidecarClient?.destroy()
		this.sidecarClient = null
	}

	async configUpdated(config: CoreVideoConfig): Promise<void> {
		this.config = config
		// Plugin
		this.clearPluginReconnect()
		this.pluginSocket?.destroy()
		this.pluginSocket = null
		this.connectPlugin()
		// OBS
		this.obsClient?.destroy()
		this.obsClient = null
		if (config.obsEnabled !== false) {
			this.connectOBS()
		} else {
			this.state.obs.connected = false
			this.checkFeedbacks('obs_connected')
		}
		// Sidecar
		this.sidecarClient?.destroy()
		this.sidecarClient = null
		if (config.sidecarEnabled) {
			this.connectSidecar()
		} else {
			this.state.sidecar.connected = false
			this.checkFeedbacks('sidecar_connected')
		}
	}

	getConfigFields(): SomeCompanionConfigField[] {
		return configFields
	}

	// ── Plugin TCP ─────────────────────────────────────────────────────────────

	private connectPlugin(): void {
		if (this.destroyed) return
		this.pluginSocket = net.createConnection({
			host: this.config.pluginHost ?? '127.0.0.1',
			port: this.config.pluginPort ?? 19870,
		})
		this.pluginSocket.setKeepAlive(true, 15000)
		this.pluginSocket.setEncoding('utf8')
		this.pluginBuffer = ''

		this.pluginSocket.on('connect', () => {
			this.updateStatus(InstanceStatus.Ok)
			this.log('info', `Plugin connected (${this.config.pluginHost}:${this.config.pluginPort})`)
			this.sendPlugin({ cmd: 'subscribe_events' })
			this.sendPlugin({ cmd: 'status' })
			this.sendPlugin({ cmd: 'list_outputs' })
			this.sendPlugin({ cmd: 'list_participants' })
		})

		this.pluginSocket.on('data', (chunk: string) => {
			this.pluginBuffer += chunk
			const lines = this.pluginBuffer.split('\n')
			this.pluginBuffer = lines.pop() ?? ''
			for (const line of lines) {
				const t = line.trim()
				if (t) this.handlePluginMessage(t)
			}
		})

		this.pluginSocket.on('error', (err) => {
			this.updateStatus(InstanceStatus.ConnectionFailure, err.message)
			this.log('debug', `Plugin error: ${err.message}`)
			this.schedulePluginReconnect()
		})

		this.pluginSocket.on('close', () => {
			if (!this.destroyed) {
				this.updateStatus(InstanceStatus.Disconnected)
				this.schedulePluginReconnect()
			}
		})
	}

	private schedulePluginReconnect(): void {
		if (this.destroyed || this.pluginReconnect) return
		this.pluginReconnect = setTimeout(() => {
			this.pluginReconnect = null
			if (!this.destroyed) this.connectPlugin()
		}, 5000)
	}

	private clearPluginReconnect(): void {
		if (this.pluginReconnect) {
			clearTimeout(this.pluginReconnect)
			this.pluginReconnect = null
		}
	}

	public sendPlugin(cmd: Record<string, unknown>): void {
		if (!this.pluginSocket?.writable) return
		if (this.config.pluginToken) cmd['token'] = this.config.pluginToken
		try { this.pluginSocket.write(JSON.stringify(cmd) + '\n') } catch { /* closed */ }
	}

	private handlePluginMessage(line: string): void {
		let msg: Record<string, unknown>
		try { msg = JSON.parse(line) as Record<string, unknown> } catch { return }

		if (typeof msg['event'] === 'string') { this.handlePluginEvent(msg); return }

		if (msg['ok'] === true) {
			if (typeof msg['meeting_state'] === 'string')
				this.state.zoom.meetingState = msg['meeting_state'] as ModuleState['zoom']['meetingState']
			if (typeof msg['active_speaker_id'] === 'number')
				this.state.zoom.activeSpeakerId = msg['active_speaker_id']
			if (Array.isArray(msg['participants']))
				this.state.zoom.participants = msg['participants'] as ModuleState['zoom']['participants']
			if (Array.isArray(msg['outputs'])) {
				this.state.zoom.outputs = msg['outputs'] as ModuleState['zoom']['outputs']
				this.setVariableDefinitions([
					...variableDefinitions,
					...buildOutputVariableDefs(this.state.zoom.outputs.length),
				])
			}
			this.flushState()
		}
	}

	private handlePluginEvent(msg: Record<string, unknown>): void {
		switch (msg['event']) {
			case 'meeting_state':
				this.state.zoom.meetingState = msg['state'] as ModuleState['zoom']['meetingState']
				this.checkFeedbacks('zoom_meeting_state', 'zoom_meeting_state_color', 'zoom_recovery_active')
				break
			case 'active_speaker':
				this.state.zoom.activeSpeakerId  = (msg['user_id'] as number) ?? 0
				this.state.zoom.activeSpeakerName = (msg['name'] as string) ?? ''
				this.checkFeedbacks('zoom_active_speaker')
				break
			case 'roster_changed':
				this.sendPlugin({ cmd: 'list_participants' })
				break
			case 'output_changed':
				this.sendPlugin({ cmd: 'list_outputs' })
				break
		}
		this.flushState()
	}

	// ── OBS WebSocket ──────────────────────────────────────────────────────────

	private connectOBS(): void {
		if (this.destroyed) return
		this.obsClient = new OBSWebSocketClient(this)
		this.obsClient.connect(
			this.config.obsHost ?? '127.0.0.1',
			this.config.obsPort ?? 4455,
			this.config.obsPassword ?? '',
		)
	}

	public obsRequest<T = unknown>(type: string, data?: Record<string, unknown>): Promise<T> {
		if (!this.obsClient) return Promise.reject(new Error('OBS not configured'))
		return this.obsClient.request<T>(type, data)
	}

	public onOBSConnected(): void {
		this.state.obs.connected = true
		this.checkFeedbacks('obs_connected')

		this.obsRequest<{ scenes: Array<{ sceneName: string }>; currentProgramSceneName: string }>(
			'GetSceneList'
		).then((d) => {
			this.state.obs.scenes = d.scenes.map((s) => s.sceneName)
			this.state.obs.currentScene = d.currentProgramSceneName
			this.setActionDefinitions(buildActions(this))
			this.setFeedbackDefinitions(buildFeedbacks(this))
			this.flushState()
		}).catch(() => {})

		this.obsRequest<{ outputActive: boolean; outputPaused?: boolean }>('GetRecordStatus').then((d) => {
			this.state.obs.recording = d.outputActive
			this.state.obs.recordingPaused = d.outputPaused === true
			this.flushState()
		}).catch(() => {})

		this.obsRequest<{ outputActive: boolean }>('GetStreamStatus').then((d) => {
			this.state.obs.streaming = d.outputActive
			this.flushState()
		}).catch(() => {})

		this.obsRequest<{ outputActive: boolean }>('GetVirtualCamStatus').then((d) => {
			this.state.obs.virtualCam = d.outputActive
			this.flushState()
		}).catch(() => {})
	}

	public onOBSDisconnected(): void {
		this.state.obs.connected = false
		this.state.obs.recording = false
		this.state.obs.recordingPaused = false
		this.state.obs.streaming = false
		this.state.obs.virtualCam = false
		this.flushState()
	}

	public onOBSEvent(eventType: string, eventData: Record<string, unknown>): void {
		switch (eventType) {
			case 'CurrentProgramSceneChanged':
				this.state.obs.currentScene = (eventData['sceneName'] as string) ?? ''
				this.checkFeedbacks('obs_current_scene')
				break
			case 'SceneListChanged':
				this.obsRequest<{ scenes: Array<{ sceneName: string }> }>('GetSceneList')
					.then((d) => {
						this.state.obs.scenes = d.scenes.map((s) => s.sceneName)
						this.setActionDefinitions(buildActions(this))
						this.setFeedbackDefinitions(buildFeedbacks(this))
					}).catch(() => {})
				break
			case 'RecordStateChanged':
				this.state.obs.recording = (eventData['outputActive'] as boolean) ?? false
				this.state.obs.recordingPaused = (eventData['outputPaused'] as boolean) ?? false
				this.checkFeedbacks('obs_recording', 'obs_record_pause')
				break
			case 'StreamStateChanged':
				this.state.obs.streaming = (eventData['outputActive'] as boolean) ?? false
				this.checkFeedbacks('obs_streaming')
				break
			case 'VirtualcamStateChanged':
				this.state.obs.virtualCam = (eventData['outputActive'] as boolean) ?? false
				this.checkFeedbacks('obs_virtual_cam')
				break
		}
		this.flushState()
	}

	// ── Sidecar TCP ────────────────────────────────────────────────────────────

	private connectSidecar(): void {
		if (this.destroyed) return
		this.sidecarClient = new SidecarClient(this)
		this.sidecarClient.connect(
			this.config.sidecarHost ?? '127.0.0.1',
			this.config.sidecarPort ?? 19880,
		)
	}

	public sendSidecar(cmd: Record<string, unknown>): void {
		this.sidecarClient?.send(cmd)
	}

	public onSidecarConnected(): void {
		this.state.sidecar.connected = true
		this.checkFeedbacks('sidecar_connected')
		this.flushState()
	}

	public onSidecarDisconnected(): void {
		this.state.sidecar.connected = false
		this.checkFeedbacks('sidecar_connected')
		this.flushState()
	}

	public onSidecarMessage(line: string): void {
		let msg: Record<string, unknown>
		try { msg = JSON.parse(line) as Record<string, unknown> } catch { return }

		if (typeof msg['event'] === 'string') { this.handleSidecarEvent(msg); return }

		if (msg['ok'] === true) {
			if (typeof msg['phase'] === 'string')
				this.state.sidecar.phase = msg['phase'] as ModuleState['sidecar']['phase']
			if (typeof msg['template_id'] === 'string')
				this.state.sidecar.templateId = msg['template_id']
			if (typeof msg['template_name'] === 'string')
				this.state.sidecar.templateName = msg['template_name']
			if (typeof msg['obs_state'] === 'string')
				this.state.sidecar.obsState = msg['obs_state']
			if (typeof msg['current_scene'] === 'string')
				this.state.sidecar.currentScene = msg['current_scene']
			if (Array.isArray(msg['templates']))
				this.state.sidecar.templates = msg['templates'] as ModuleState['sidecar']['templates']
			if (Array.isArray(msg['scenes']))
				this.state.sidecar.scenes = msg['scenes'] as string[]
			this.setActionDefinitions(buildActions(this))
			this.setFeedbackDefinitions(buildFeedbacks(this))
			this.flushState()
		}
	}

	private handleSidecarEvent(msg: Record<string, unknown>): void {
		switch (msg['event']) {
			case 'phase_changed':
				this.state.sidecar.phase = msg['phase'] as ModuleState['sidecar']['phase']
				this.checkFeedbacks('show_phase', 'show_phase_color')
				break
			case 'template_changed':
				this.state.sidecar.templateId   = (msg['template_id'] as string) ?? ''
				this.state.sidecar.templateName = (msg['template_name'] as string) ?? ''
				this.checkFeedbacks('show_template')
				break
			case 'obs_state':
				this.state.sidecar.obsState = (msg['state'] as string) ?? 'disconnected'
				break
			case 'scene_changed':
				this.state.sidecar.currentScene = (msg['scene'] as string) ?? ''
				break
			case 'scenes_updated':
				this.state.sidecar.scenes = (msg['scenes'] as string[]) ?? []
				this.setActionDefinitions(buildActions(this))
				break
		}
		this.flushState()
	}

	// ── State flush ────────────────────────────────────────────────────────────

	private flushState(): void {
		this.setVariableValues(buildVariableValues(this.state))
		this.checkFeedbacks(
			'zoom_meeting_state', 'zoom_meeting_state_color',
			'zoom_active_speaker', 'zoom_output_assigned', 'zoom_recovery_active',
			'obs_connected', 'obs_recording', 'obs_streaming', 'obs_virtual_cam',
			'obs_current_scene', 'obs_record_pause',
			'sidecar_connected', 'show_phase', 'show_phase_color', 'show_template',
		)
	}
}

runEntrypoint(CoreVideoInstance, [])
