import type {
	CompanionActionDefinitions,
	CompanionActionSchemaWithoutResult,
	CompanionOptionValues,
} from '@companion-module/base'
import type { CoreVideoInstance } from './index.js'

export function buildActions(
	inst: CoreVideoInstance,
): CompanionActionDefinitions<Record<string, CompanionActionSchemaWithoutResult<CompanionOptionValues>>> {
	// The outputs the plugin currently knows about, as OBS source names. These
	// are stable across meetings (they are OBS sources), so storing one on a
	// button is safe.
	const outputChoices = inst.state.zoom.outputs.map((o) => ({
		id: o.source,
		label: o.display_name ? `${o.source} (${o.display_name})` : o.source,
	}))

	// Participants are offered BY NAME, not by id. Zoom user IDs are
	// meeting-scoped: the same person rejoining, or the next run of a weekly
	// show, gets a brand new id, so a button holding a raw id silently points
	// at nobody -- or, once ids get reused, at the wrong face. Names survive
	// that.
	const participantChoices = inst.state.zoom.participants.map((p) => ({
		id: p.name,
		label: p.is_muted ? `${p.name} (muted)` : p.name,
	}))

	// Resolve whatever the button stored into a live participant id.
	// Accepts, in order: a name from the dropdown, a raw numeric id typed in
	// via allowCustom, or a legacy numeric participant_id from a button built
	// before this action offered a dropdown.
	const resolveParticipantId = (choice: unknown, legacyId: unknown): number => {
		const raw = String(choice ?? '').trim()
		if (raw) {
			if (/^\d+$/.test(raw)) return Number(raw)
			const hit = inst.state.zoom.participants.find(
				(p) => p.name.toLowerCase() === raw.toLowerCase(),
			)
			if (hit) return hit.id
			// Named someone who is not in the meeting right now. Return 0
			// rather than a stale guess: the plugin treats 0 as "nobody",
			// which shows the source as unassigned instead of putting the
			// wrong person on air.
			inst.log('warn', `Assign: no participant named "${raw}" in the current roster`)
			return 0
		}
		return Number(legacyId ?? 0) || 0
	}

	return {

		// ── Zoom: Meeting ───────────────────────────────────────────────────────

		zoom_join: {
			name: 'Zoom: Join Meeting',
			options: [
				{ type: 'textinput', id: 'meeting_id',   label: 'Meeting ID or Zoom URL', default: '' },
				{ type: 'textinput', id: 'passcode',     label: 'Passcode (optional)',    default: '' },
				{ type: 'textinput', id: 'display_name', label: 'Display Name',           default: 'OBS' },
			],
			callback: (a) => inst.sendPlugin({ cmd: 'join', meeting_id: a.options.meeting_id,
				passcode: a.options.passcode, display_name: a.options.display_name }),
		},

		zoom_leave: {
			name: 'Zoom: Leave Meeting',
			options: [],
			callback: () => inst.sendPlugin({ cmd: 'leave' }),
		},

		zoom_assign: {
			name: 'Zoom: Assign Participant to Output',
			options: [
				{
					type: 'dropdown', id: 'source', label: 'Output (OBS source)',
					default: '', allowCustom: true, choices: outputChoices,
				},
				{
					type: 'dropdown', id: 'participant', label: 'Participant (by name)',
					default: '', allowCustom: true, choices: participantChoices,
				},
				{ type: 'checkbox',  id: 'active_speaker', label: 'Track Active Speaker instead', default: false },
				{ type: 'checkbox',  id: 'isolate_audio',  label: 'Isolate Audio',                default: false },
			],
			callback: (a) => inst.sendPlugin({ cmd: 'assign_output',
				source: a.options.source,
				// Active-speaker tracking ignores the participant entirely, so
				// don't resolve a name (and don't warn about one) in that case.
				participant_id: a.options.active_speaker
					? 0
					: resolveParticipantId(a.options.participant, a.options.participant_id),
				active_speaker: a.options.active_speaker,
				isolate_audio: a.options.isolate_audio }),
		},

		zoom_assign_spotlight: {
			name: 'Zoom: Assign Spotlight Slot to Output',
			options: [
				{
					type: 'dropdown', id: 'source', label: 'Output (OBS source)',
					default: '', allowCustom: true, choices: outputChoices,
				},
				{ type: 'number',    id: 'slot',   label: 'Spotlight Slot (1-based)', default: 1, min: 1, max: 49 },
			],
			callback: (a) => inst.sendPlugin({ cmd: 'assign_output_ex', source: a.options.source,
				mode: 'spotlight', spotlight_slot: a.options.slot }),
		},

		zoom_assign_screen_share: {
			name: 'Zoom: Assign Screen Share to Output',
			options: [
				{
					type: 'dropdown', id: 'source', label: 'Output (OBS source)',
					default: '', allowCustom: true, choices: outputChoices,
				},
			],
			callback: (a) => inst.sendPlugin({ cmd: 'assign_output_ex',
				source: a.options.source, mode: 'screen_share' }),
		},

		zoom_cancel_recovery: {
			name: 'Zoom: Cancel Auto-Recovery',
			options: [],
			callback: () => inst.sendPlugin({ cmd: 'recovery_cancel' }),
		},

		// ── OBS: Scenes ─────────────────────────────────────────────────────────

		obs_switch_scene: {
			name: 'OBS: Switch Scene',
			options: [
				{ type: 'textinput', id: 'scene', label: 'Scene Name', default: '' },
			],
			callback: async (a) => { await inst.obsRequest('SetCurrentProgramScene',
				{ sceneName: a.options.scene }) },
		},

		obs_switch_scene_pick: {
			name: 'OBS: Switch Scene (dropdown)',
			options: [
				{
					type: 'dropdown', id: 'scene', label: 'Scene',
					default: '', allowCustom: true,
					choices: inst.state.obs.scenes.map((s) => ({ id: s, label: s })),
				},
			],
			callback: async (a) => { await inst.obsRequest('SetCurrentProgramScene',
				{ sceneName: a.options.scene }) },
		},

		// ── OBS: Recording ──────────────────────────────────────────────────────

		obs_start_record: {
			name: 'OBS: Start Recording',
			options: [],
			callback: async () => { await inst.obsRequest('StartRecord') },
		},

		obs_stop_record: {
			name: 'OBS: Stop Recording',
			options: [],
			callback: async () => { await inst.obsRequest('StopRecord') },
		},

		obs_toggle_record: {
			name: 'OBS: Toggle Recording',
			options: [],
			callback: async () => { await inst.obsRequest('ToggleRecord') },
		},

		obs_pause_record: {
			name: 'OBS: Pause / Resume Recording',
			options: [],
			callback: async () => { await inst.obsRequest('ToggleRecordPause') },
		},

		// ── OBS: Streaming ──────────────────────────────────────────────────────

		obs_start_stream: {
			name: 'OBS: Start Streaming',
			options: [],
			callback: async () => { await inst.obsRequest('StartStream') },
		},

		obs_stop_stream: {
			name: 'OBS: Stop Streaming',
			options: [],
			callback: async () => { await inst.obsRequest('StopStream') },
		},

		obs_toggle_stream: {
			name: 'OBS: Toggle Streaming',
			options: [],
			callback: async () => { await inst.obsRequest('ToggleStream') },
		},

		// ── OBS: Virtual Camera ─────────────────────────────────────────────────

		obs_start_vcam: {
			name: 'OBS: Start Virtual Camera',
			options: [],
			callback: async () => { await inst.obsRequest('StartVirtualCam') },
		},

		obs_stop_vcam: {
			name: 'OBS: Stop Virtual Camera',
			options: [],
			callback: async () => { await inst.obsRequest('StopVirtualCam') },
		},

		obs_toggle_vcam: {
			name: 'OBS: Toggle Virtual Camera',
			options: [],
			callback: async () => { await inst.obsRequest('ToggleVirtualCam') },
		},

		// ── OBS: Source visibility ──────────────────────────────────────────────

		obs_source_visible: {
			name: 'OBS: Set Source Visibility',
			options: [
				{ type: 'textinput', id: 'scene',   label: 'Scene Name',  default: '' },
				{ type: 'textinput', id: 'source',  label: 'Source Name', default: '' },
				{
					type: 'dropdown', id: 'enabled', label: 'Visibility',
					default: 'true',
					choices: [{ id: 'true', label: 'Visible' }, { id: 'false', label: 'Hidden' }],
				},
			],
			callback: async (a) => {
				const scene  = a.options.scene  as string || inst.state.obs.currentScene
				const source = a.options.source as string
				// Look up sceneItemId first
				const data = await inst.obsRequest<{ sceneItems: Array<{ sceneItemId: number; sourceName: string }> }>(
					'GetSceneItemList', { sceneName: scene })
				const item = data?.sceneItems?.find((i) => i.sourceName === source)
				if (!item) { inst.log('warn', `OBS source "${source}" not found in scene "${scene}"`); return }
				inst.obsRequest('SetSceneItemEnabled', {
					sceneName: scene, sceneItemId: item.sceneItemId,
					sceneItemEnabled: a.options.enabled === 'true',
				})
			},
		},

		// ── Show: Phases (Sidecar) ──────────────────────────────────────────────

		show_set_phase: {
			name: 'Show: Set Phase',
			options: [
				{
					type: 'dropdown', id: 'phase', label: 'Phase', default: 'live',
					choices: [
						{ id: 'pre_show',  label: 'Pre-Show' },
						{ id: 'live',      label: '● LIVE'   },
						{ id: 'post_show', label: 'Post-Show' },
					],
				},
			],
			callback: (a) => inst.sendSidecar({ cmd: 'set_phase', phase: a.options.phase }),
		},

		show_apply_template: {
			name: 'Show: Apply Layout Template',
			options: [
				{
					type: 'dropdown', id: 'template_id', label: 'Template',
					default: '4-up-grid', allowCustom: true,
					choices: inst.state.sidecar.templates.map((t) => ({ id: t.id, label: t.name })),
				},
			],
			callback: (a) => inst.sendSidecar({ cmd: 'apply_template',
				template_id: a.options.template_id }),
		},

		show_set_scene: {
			name: 'Show: Switch OBS Scene (via Sidecar)',
			options: [
				{
					type: 'dropdown', id: 'scene', label: 'Scene', default: '', allowCustom: true,
					choices: inst.state.sidecar.scenes.map((s) => ({ id: s, label: s })),
				},
			],
			callback: (a) => inst.sendSidecar({ cmd: 'set_scene', scene: a.options.scene }),
		},
	}
}
