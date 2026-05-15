// ── Zoom / CoreVideo plugin ──────────────────────────────────────────────────

export type MeetingState = 'idle' | 'joining' | 'in_meeting' | 'leaving' | 'recovering' | 'failed'

export interface Participant {
	id: number
	name: string
	has_video: boolean
	is_talking: boolean
	is_muted: boolean
}

export interface Output {
	source: string
	display_name: string
	participant_id: number
	active_speaker: boolean
	assignment_mode: 'participant' | 'active_speaker' | 'spotlight' | 'screen_share'
	spotlight_slot: number
	isolate_audio: boolean
	audio_channels: 'mono' | 'stereo'
}

export type ShowPhase = 'pre_show' | 'live' | 'post_show'

// ── Combined module state ────────────────────────────────────────────────────

export interface ModuleState {
	zoom: {
		meetingState: MeetingState
		activeSpeakerId: number
		activeSpeakerName: string
		participants: Participant[]
		outputs: Output[]
	}
	obs: {
		connected: boolean
		currentScene: string
		scenes: string[]
		recording: boolean
		recordingPaused: boolean
		streaming: boolean
		virtualCam: boolean
	}
	sidecar: {
		connected: boolean
		phase: ShowPhase
		templateId: string
		templateName: string
		templates: Array<{ id: string; name: string }>
		obsState: string
		currentScene: string
		scenes: string[]
	}
}

export function defaultState(): ModuleState {
	return {
		zoom: {
			meetingState: 'idle',
			activeSpeakerId: 0,
			activeSpeakerName: '',
			participants: [],
			outputs: [],
		},
		obs: {
			connected: false,
			currentScene: '',
			scenes: [],
			recording: false,
			recordingPaused: false,
			streaming: false,
			virtualCam: false,
		},
		sidecar: {
			connected: false,
			phase: 'pre_show',
			templateId: '',
			templateName: '',
			templates: [],
			obsState: 'disconnected',
			currentScene: '',
			scenes: [],
		},
	}
}
