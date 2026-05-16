import { combineRgb } from '@companion-module/base'
import type { CompanionFeedbackDefinitions } from '@companion-module/base'
import type { CoreVideoInstance } from './index.js'
import type { MeetingState, ShowPhase } from './state.js'

const GREEN  = combineRgb(0,   196, 79)
const AMBER  = combineRgb(230, 160, 32)
const RED    = combineRgb(204, 32,  32)
const BLUE   = combineRgb(41,  121, 255)
const GREY   = combineRgb(40,  40,  60)
const WHITE  = combineRgb(255, 255, 255)
const BLACK  = combineRgb(0,   0,   0)

const ZOOM_STATE_COLORS: Record<MeetingState, number> = {
	in_meeting: GREEN, joining: AMBER, recovering: AMBER,
	leaving: AMBER, failed: RED, idle: GREY,
}

const PHASE_COLORS: Record<ShowPhase, number> = {
	pre_show: GREY, live: RED, post_show: BLUE,
}

export function buildFeedbacks(inst: CoreVideoInstance): CompanionFeedbackDefinitions {
	return {

		// ── Zoom ────────────────────────────────────────────────────────────────

		zoom_meeting_state: {
			type: 'boolean',
			name: 'Zoom: Meeting State',
			description: 'True when the Zoom meeting is in the given state',
			defaultStyle: { bgcolor: GREEN, color: BLACK },
			options: [
				{
					type: 'dropdown', id: 'state', label: 'State', default: 'in_meeting',
					choices: [
						{ id: 'in_meeting', label: '● In Meeting'  },
						{ id: 'joining',    label: '⟳ Joining'    },
						{ id: 'recovering', label: '⟳ Recovering' },
						{ id: 'leaving',    label: '⟳ Leaving'    },
						{ id: 'failed',     label: '✕ Failed'      },
						{ id: 'idle',       label: '○ Idle'        },
					],
				},
			],
			callback: (fb) => inst.state.zoom.meetingState === fb.options['state'],
		},

		zoom_meeting_state_color: {
			type: 'advanced',
			name: 'Zoom: Meeting State Color',
			description: 'Button tracks the current Zoom state with a dynamic color',
			options: [],
			callback: () => ({
				bgcolor: ZOOM_STATE_COLORS[inst.state.zoom.meetingState] ?? GREY,
				color: WHITE,
				text: inst.state.zoom.meetingState.replace('_', ' ').toUpperCase(),
			}),
		},

		zoom_active_speaker: {
			type: 'boolean',
			name: 'Zoom: Is Active Speaker',
			description: 'True when the participant with the given ID is speaking',
			defaultStyle: { bgcolor: BLUE, color: WHITE },
			options: [
				{ type: 'number', id: 'participant_id', label: 'Participant ID', default: 0, min: 0, max: 999999999 },
			],
			callback: (fb) => inst.state.zoom.activeSpeakerId === (fb.options['participant_id'] as number),
		},

		zoom_output_assigned: {
			type: 'boolean',
			name: 'Zoom: Output Has Participant',
			description: 'True when the named OBS source has a participant assigned',
			defaultStyle: { bgcolor: BLUE, color: WHITE },
			options: [
				{ type: 'textinput', id: 'source', label: 'OBS Source Name', default: '' },
			],
			callback: (fb) => {
				const out = inst.state.zoom.outputs.find((o) => o.source === fb.options['source'])
				return !!out && out.participant_id > 0
			},
		},

		zoom_recovery_active: {
			type: 'boolean',
			name: 'Zoom: Auto-Recovery Active',
			description: 'True while CoreVideo is attempting to reconnect',
			defaultStyle: { bgcolor: AMBER, color: BLACK },
			options: [],
			callback: () => inst.state.zoom.meetingState === 'recovering',
		},

		// ── OBS ─────────────────────────────────────────────────────────────────

		obs_connected: {
			type: 'boolean',
			name: 'OBS: Connected',
			description: 'True while the OBS WebSocket connection is active',
			defaultStyle: { bgcolor: GREEN, color: BLACK },
			options: [],
			callback: () => inst.state.obs.connected,
		},

		obs_recording: {
			type: 'boolean',
			name: 'OBS: Recording',
			description: 'True while OBS is recording',
			defaultStyle: { bgcolor: RED, color: WHITE },
			options: [],
			callback: () => inst.state.obs.recording,
		},

		obs_streaming: {
			type: 'boolean',
			name: 'OBS: Streaming',
			description: 'True while OBS is live',
			defaultStyle: { bgcolor: RED, color: WHITE },
			options: [],
			callback: () => inst.state.obs.streaming,
		},

		obs_virtual_cam: {
			type: 'boolean',
			name: 'OBS: Virtual Camera Active',
			description: 'True while OBS virtual camera is on',
			defaultStyle: { bgcolor: GREEN, color: BLACK },
			options: [],
			callback: () => inst.state.obs.virtualCam,
		},

		obs_current_scene: {
			type: 'boolean',
			name: 'OBS: Current Scene',
			description: 'True when the named scene is the current program scene',
			defaultStyle: { bgcolor: GREEN, color: BLACK },
			options: [
				{
					type: 'dropdown', id: 'scene', label: 'Scene', default: '', allowCustom: true,
					choices: inst.state.obs.scenes.map((s) => ({ id: s, label: s })),
				},
			],
			callback: (fb) => inst.state.obs.currentScene === fb.options['scene'],
		},

		obs_record_pause: {
			type: 'boolean',
			name: 'OBS: Recording Paused',
			description: 'True while recording is paused',
			defaultStyle: { bgcolor: AMBER, color: BLACK },
			options: [],
			callback: () => inst.state.obs.recordingPaused,
		},

		// ── Sidecar / Show ───────────────────────────────────────────────────────

		sidecar_connected: {
			type: 'boolean',
			name: 'Sidecar: Connected',
			description: 'True while the Sidecar app is connected',
			defaultStyle: { bgcolor: GREEN, color: BLACK },
			options: [],
			callback: () => inst.state.sidecar.connected,
		},

		show_phase: {
			type: 'boolean',
			name: 'Show: Phase',
			description: 'True when the show is in the selected phase',
			defaultStyle: { bgcolor: RED, color: WHITE },
			options: [
				{
					type: 'dropdown', id: 'phase', label: 'Phase', default: 'live',
					choices: [
						{ id: 'pre_show',  label: 'Pre-Show'   },
						{ id: 'live',      label: '● LIVE'     },
						{ id: 'post_show', label: 'Post-Show'  },
					],
				},
			],
			callback: (fb) => inst.state.sidecar.phase === fb.options['phase'],
		},

		show_phase_color: {
			type: 'advanced',
			name: 'Show: Phase Color (dynamic)',
			description: 'Button color tracks PRE (grey) / LIVE (red) / POST (blue)',
			options: [],
			callback: () => ({
				bgcolor: PHASE_COLORS[inst.state.sidecar.phase] ?? GREY,
				color: WHITE,
				text: inst.state.sidecar.phase === 'live'
					? '● LIVE'
					: inst.state.sidecar.phase.replace('_', '-').toUpperCase(),
			}),
		},

		show_template: {
			type: 'boolean',
			name: 'Show: Active Template',
			description: 'True when the named template is currently applied in Sidecar',
			defaultStyle: { bgcolor: BLUE, color: WHITE },
			options: [
				{ type: 'textinput', id: 'template_id', label: 'Template ID', default: '4-up-grid' },
			],
			callback: (fb) => inst.state.sidecar.templateId === fb.options['template_id'],
		},
	}
}
