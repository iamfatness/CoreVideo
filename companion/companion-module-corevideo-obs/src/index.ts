import {
	InstanceBase,
	InstanceStatus,
	runEntrypoint,
	type SomeCompanionConfigField,
} from '@companion-module/base'
import * as net from 'net'
import { configFields, type CoreVideoConfig } from './config.js'
import { defaultState, type CoreVideoState } from './state.js'
import { buildActions } from './actions.js'
import { buildFeedbacks } from './feedbacks.js'
import {
	variableDefinitions,
	buildVariableValues,
	buildOutputVariableDefs,
} from './variables.js'

export class CoreVideoInstance extends InstanceBase<CoreVideoConfig> {
	public state: CoreVideoState = defaultState()

	private socket: net.Socket | null = null
	private reconnectTimer: ReturnType<typeof setTimeout> | null = null
	private recvBuffer = ''
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
		this.connect()
	}

	async destroy(): Promise<void> {
		this.destroyed = true
		this.clearReconnect()
		this.socket?.destroy()
		this.socket = null
	}

	async configUpdated(config: CoreVideoConfig): Promise<void> {
		this.config = config
		this.socket?.destroy()
		this.socket = null
		this.clearReconnect()
		this.connect()
	}

	getConfigFields(): SomeCompanionConfigField[] {
		return configFields
	}

	// ── TCP Connection ─────────────────────────────────────────────────────────

	private connect(): void {
		if (this.destroyed) return

		this.socket = net.createConnection({
			host: this.config.host ?? '127.0.0.1',
			port: this.config.port ?? 19870,
		})

		this.socket.setKeepAlive(true, 15000)
		this.socket.setEncoding('utf8')
		this.recvBuffer = ''

		this.socket.on('connect', () => {
			this.updateStatus(InstanceStatus.Ok)
			this.log('info', `Connected to CoreVideo at ${this.config.host}:${this.config.port}`)
			// Subscribe to push events and pull initial state.
			this.sendCmd({ cmd: 'subscribe_events' })
			this.sendCmd({ cmd: 'status' })
			this.sendCmd({ cmd: 'list_outputs' })
			this.sendCmd({ cmd: 'list_participants' })
		})

		this.socket.on('data', (chunk: string) => {
			this.recvBuffer += chunk
			const lines = this.recvBuffer.split('\n')
			this.recvBuffer = lines.pop() ?? ''
			for (const line of lines) {
				if (line.trim()) this.handleMessage(line.trim())
			}
		})

		this.socket.on('error', (err) => {
			this.updateStatus(InstanceStatus.ConnectionFailure, err.message)
			this.log('debug', `Connection error: ${err.message}`)
			this.scheduleReconnect()
		})

		this.socket.on('close', () => {
			if (!this.destroyed) {
				this.updateStatus(InstanceStatus.Disconnected)
				this.scheduleReconnect()
			}
		})
	}

	private scheduleReconnect(): void {
		if (this.destroyed || this.reconnectTimer) return
		this.reconnectTimer = setTimeout(() => {
			this.reconnectTimer = null
			if (!this.destroyed) this.connect()
		}, 5000)
	}

	private clearReconnect(): void {
		if (this.reconnectTimer) {
			clearTimeout(this.reconnectTimer)
			this.reconnectTimer = null
		}
	}

	// ── Protocol ───────────────────────────────────────────────────────────────

	public sendCmd(cmd: Record<string, unknown>): void {
		if (!this.socket?.writable) return
		if (this.config.token) cmd['token'] = this.config.token
		try {
			this.socket.write(JSON.stringify(cmd) + '\n')
		} catch {
			// socket may have closed between the writable check and write
		}
	}

	private handleMessage(line: string): void {
		let msg: Record<string, unknown>
		try {
			msg = JSON.parse(line) as Record<string, unknown>
		} catch {
			this.log('debug', `Unparseable message: ${line}`)
			return
		}

		// Push events from subscribe_events subscription
		if (typeof msg['event'] === 'string') {
			this.handleEvent(msg)
			return
		}

		// Responses to list_* / status commands
		if (msg['ok'] === true) {
			if (typeof msg['meeting_state'] === 'string') {
				this.state.meetingState = msg['meeting_state'] as CoreVideoState['meetingState']
			}
			if (typeof msg['active_speaker_id'] === 'number') {
				this.state.activeSpeakerId = msg['active_speaker_id']
			}
			if (Array.isArray(msg['participants'])) {
				this.state.participants = msg['participants'] as CoreVideoState['participants']
			}
			if (Array.isArray(msg['outputs'])) {
				this.state.outputs = msg['outputs'] as CoreVideoState['outputs']
				// Rebuild per-output variable definitions when output count changes.
				this.setVariableDefinitions([
					...variableDefinitions,
					...buildOutputVariableDefs(this.state.outputs.length),
				])
			}
			this.flushState()
		}
	}

	private handleEvent(msg: Record<string, unknown>): void {
		switch (msg['event']) {
			case 'meeting_state':
				this.state.meetingState = msg['state'] as CoreVideoState['meetingState']
				this.checkFeedbacks('meeting_state', 'meeting_state_color', 'recovery_active')
				break

			case 'active_speaker':
				this.state.activeSpeakerId  = (msg['user_id'] as number) ?? 0
				this.state.activeSpeakerName = (msg['name'] as string) ?? ''
				this.checkFeedbacks('active_speaker_id')
				break

			case 'roster_changed':
				// Roster changed — re-fetch full list.
				this.sendCmd({ cmd: 'list_participants' })
				break

			case 'output_changed':
				this.sendCmd({ cmd: 'list_outputs' })
				break
		}

		this.flushState()
	}

	private flushState(): void {
		this.setVariableValues(buildVariableValues(this.state))
		this.checkFeedbacks(
			'meeting_state',
			'meeting_state_color',
			'active_speaker_id',
			'output_assigned',
			'output_active_speaker',
			'recovery_active',
		)
	}
}

runEntrypoint(CoreVideoInstance, [])
