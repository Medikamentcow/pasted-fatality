#include <random>
#include "../include_cheat.h"

void resolver::resolve( C_CSPlayer* player, lag_record_t* record, lag_record_t* previous )
{
	if ( !player->is_enemy() )
		return;

	if ( !record->m_shot )
		pitch_resolve( record );

	auto& log = player_log::get_log(player->EntIndex());
	update_anim_info(log, record, previous);

	yaw_resolve( record, previous );
}

void resolver::post_animate(C_CSPlayer* player, lag_record_t* record)
{
	const auto log = &player_log::get_log(player->EntIndex());

	// Only strip extras when resolver_mode limits the set.
	// Enum order: networked(0), max(1), zero(2), min(3), max_extra(4), ...
	// Do NOT use "> resolver_max" — that incorrectly wipes min/zero.
	if (vars::aim.resolver_mode->get<int>())
	{
		for (auto& mode : log->m_mode)
		{
			for (auto& side : mode.m_side)
			{
				if (side.m_current_dir >= resolver_direction::resolver_max_extra)
				{
					side.m_current_dir = resolver_direction::resolver_networked;
					log->m_unknown_shot = true;
					log->m_unknown = true;
				}
			}
		}
	}

	if (!player->is_enemy() || player->get_player_info().fakeplayer)
	{
		log->m_mode[resolver_mode::resolver_shot].m_side =
			log->m_mode[resolver_mode::resolver_default].m_side =
			log->m_mode[resolver_mode::resolver_flip].m_side = {};
	}

	record->m_resolver_mode = record->m_shot ? resolver_mode::resolver_shot : log->m_current_mode;
	record->m_resolver_side = log->m_current_side;

	if (!record->m_shot)
	{
		const auto cureye = record->m_eye_angles;
		if (fabsf(cureye.x) >= 60.f)
			log->m_last_unusual_pitch = interfaces::globals()->curtime;
		else
			log->m_last_zero_pitch = interfaces::globals()->curtime;
	}

	// Copy resolved dir into shot-mode slot when still unknown on shots
	const auto& live_dir =
		log->m_mode[log->m_current_mode].m_side[log->m_current_side].m_current_dir;

	if (log->m_unknown_shot && live_dir > resolver_direction::resolver_networked)
	{
		log->m_mode[resolver_mode::resolver_shot]
			.m_side[log->m_current_side]
			.m_current_dir = live_dir;
	}
}

bool resolver::extrapolate_record( int ticks, lag_record_t& outrecord, const bool simple )
{
	if ( !ticks )
	{
		outrecord.setup_matrices();
		return true;
	}

	const auto player = globals::get_player( outrecord.m_index );

	const auto backup_lby = player->get_lby();
	const auto backup_layers = player->get_anim_layers();
	const auto backup_state = *player->get_anim_state();
	const auto backup_poses = player->get_pose_params();
	const auto backup_angle = player->get_abs_rotation();

	const auto backup_abs_origin = player->get_abs_origin();
	const auto backup_flags = player->get_flags();
	const auto backup_groundentity = player->get_ground_entity();
	const auto backup_move_type = player->get_move_type();
	const auto backup_velocity = player->get_velocity();
	const auto backup_ducking = player->get_ducking();

	outrecord.m_velocity = outrecord.m_calculated_velocity;

	player->get_velocity().z = outrecord.m_calculated_velocity.z;

	auto new_previous = std::make_unique<lag_record_t>();
	*new_previous = outrecord;
	new_previous->m_extrapolated = true;
	auto& log = player_log::get_log( outrecord.m_index );

	if ( simple )
	{
		process_move_changes_t backup_pm{};
		backup_pm.store( player );

		const auto original_record = log.record.back();
		const auto p1 = log.record.size() > 1 ? &log.record[ log.record.size() - 2 ] : nullptr;
		const auto p2 = log.record.size() > 2 ? &log.record[ log.record.size() - 3 ] : nullptr;

		int prev_buttons = 0;

		Vector predicted_vel_change{}, record_vel_change{};
		if ( p1 && p2 )
		{
			const auto p1_vel_change = ( p1->m_calculated_velocity - p2->m_calculated_velocity ) / p1->m_lagamt;
			record_vel_change = ( original_record.m_calculated_velocity - p1->m_calculated_velocity ) / original_record.m_lagamt;
			predicted_vel_change = record_vel_change - p1_vel_change;
		}

		const auto speed = original_record.m_velocity.Length2D();

		CUserCmd cmd{};
		for ( auto i = 0; i < ticks; i++ )
		{
			QAngle predicted_vel_change_ang;
			math::vector_angles( player->get_velocity() + predicted_vel_change, predicted_vel_change_ang );
			cmd.viewangles.y = predicted_vel_change_ang.y;
			cmd.viewangles.x = 0;

			cmd.forwardmove = speed > 5.f ? 450.f : ( i % 2 ? 1.01f : -1.01f );
			cmd.sidemove = 0.f;

			if ( original_record.m_duckamt > 0.f )
				cmd.buttons |= IN_DUCK;
			else
				cmd.buttons &= ~IN_DUCK;

			if ( i == 0 )
			{
				if ( player->get_duck_amt() > 0.f )
					player->get_ducking() = true;
				else
					player->get_ducking() = false;

				if ( player->get_duck_amt() == 1.f )
				{
					player->get_ducked() = true;
					player->get_ducking() = false;
				}
				else
					player->get_ducked() = false;

				prev_buttons = cmd.buttons;

				if ( !( player->get_flags() & FL_ONGROUND ) )
					prev_buttons |= IN_JUMP;
			}

			if ( !( player->get_flags() & FL_ONGROUND ) )
			{
				QAngle vel_ang;
				math::vector_angles( player->get_velocity(), vel_ang );

				if ( fabsf( math::normalize_float( vel_ang.y - predicted_vel_change_ang.y ) ) > 20.f )
				{
					cmd.forwardmove = 0.f;
					cmd.sidemove = fabsf( vel_ang.y - predicted_vel_change_ang.y ) > 0.f ? 450.f : -450.f;
				}
			}
			else if ( p1 && speed < p1->m_velocity.Length2D() - 5.f * original_record.m_lagamt || speed < 5.f )
			{
				CMoveData data = interfaces::game_movement()->setup_move( player, &cmd );
				aimbot_helpers::stop_to_speed( 1.01f, &data, player );
				cmd.forwardmove = data.m_flForwardMove;
				cmd.sidemove = data.m_flSideMove;
			}
			else if ( p1 && speed < p1->m_velocity.Length2D() + 5.f * original_record.m_lagamt && speed > 5.f )
			{
				CMoveData data = interfaces::game_movement()->setup_move( player, &cmd );
				aimbot_helpers::stop_to_speed( ( player->get_velocity() + predicted_vel_change ).Length2D(), &data, player );
				cmd.forwardmove = data.m_flForwardMove;
				cmd.sidemove = data.m_flSideMove;
			}

			CMoveData data = interfaces::game_movement()->setup_move( player, &cmd );
			data.m_nOldButtons = prev_buttons;
			const auto ret = interfaces::game_movement()->process_movement( player, &data );
			prev_buttons = data.m_nButtons;
			ret.restore( player );

			if ( p1 )
			{
				if ( !( p1->m_flags & FL_ONGROUND ) && !( original_record.m_flags & FL_ONGROUND ) && player->get_flags() & FL_ONGROUND )
					cmd.buttons |= IN_JUMP;
				else
					cmd.buttons &= ~IN_JUMP;
			}

			player->set_abs_origin( data.m_vecAbsOrigin );
			player->get_velocity() = data.m_vecVelocity;

			if ( i == ticks - 1 )
				outrecord.m_origin = data.m_vecAbsOrigin;
		}

		backup_pm.restore( player );
		player->set_abs_origin( backup_abs_origin );
		player->get_flags() = backup_flags;
		player->get_ground_entity() = backup_groundentity;
		player->get_move_type() = backup_move_type;
		player->get_velocity() = backup_velocity;
		player->get_ducking() = backup_ducking;

		return true;
	}

	new_previous->m_velocity = outrecord.m_calculated_velocity;
	outrecord.m_simtime += interfaces::globals()->interval_per_tick * ticks;
	outrecord.m_lagamt = ticks;
	animations::update_player_animations( &outrecord, player, new_previous.get() );

	player->get_lby() = backup_lby;
	player->get_anim_layers() = backup_layers;
	*player->get_anim_state() = backup_state;
	player->get_pose_params() = backup_poses;
	player->set_abs_angles( backup_angle );
	player->get_velocity() = backup_velocity;

	for ( auto& state : outrecord.m_state )
		state.m_setup_tick = -1;
	outrecord.setup_matrices( resolver_direction::resolver_invalid, true );

	//aimbot_helpers::draw_debug_hitboxes( player, outrecord.matrix( player_log::get_log( outrecord.m_index ).get_dir( outrecord.m_shot, outrecord.m_resolver_mode ) ), -1, interfaces::globals()->interval_per_tick * 2 );

	/*for ( auto j = resolver_direction::resolver_networked; j < resolver_direction::resolver_direction_max; j++ )
	{
		if ( j == resolver_direction::resolver_min || j == resolver_direction::resolver_max )
		{
			aimbot_helpers::draw_debug_hitboxes( player, outrecord.matrix( j ), -1, interfaces::globals()->interval_per_tick * 2, Color::Blue( 100 ) );
		}

		if ( j == resolver_direction::resolver_min_min || j == resolver_direction::resolver_max_max )
		{
			aimbot_helpers::draw_debug_hitboxes( player, outrecord.matrix( j ), -1, interfaces::globals()->interval_per_tick * 2, Color::Green( 100 ) );
		}

		if ( j == resolver_direction::resolver_min_extra )
		{
			aimbot_helpers::draw_debug_hitboxes( player, outrecord.matrix( j ), -1, interfaces::globals()->interval_per_tick * 2, Color::Red( 100 ) );
		}

	}*/

	return true;
}

void resolver::pitch_resolve( lag_record_t* record )
{
	const auto& log = player_log::get_log( record->m_index );

	if ( globals::nospread )
	{
		if ( log.nospread.m_pitch_cycle % 2 && log.nospread.m_can_fake )
		{
			record->m_eye_angles.x = -record->m_eye_angles.x;
		}
	}

	record->m_pitch_cycle = log.nospread.m_pitch_cycle;
}


float resolver::get_resolver_angle( const lag_record_t& record, resolver_direction direction, float eye_angle )
{
	switch ( direction )
	{
		case resolver_direction::resolver_max:
			return math::normalize_float( eye_angle + record.m_state[ direction ].m_animstate.aim_yaw_max * record.m_yaw_modifier * 2.f );
		case resolver_direction::resolver_min:
			return math::normalize_float( eye_angle + record.m_state[ direction ].m_animstate.aim_yaw_min * record.m_yaw_modifier * 2.f );
		default:
			return eye_angle;
	}
}

static float cycle_delta(float cur, float prev)
{
	float d = cur - prev;
	if (d > 0.5f)  d -= 1.f;
	if (d < -0.5f) d += 1.f;
	return fabsf(d);
}

void resolver::update_anim_info(player_log_t& log, lag_record_t* record, lag_record_t* previous)
{
	if (!record)
		return;

	auto& a = log.m_anim;
	auto& j = log.m_jitter;
	const auto& layers = record->m_layers;

	// ----- stash previous -----
	a.prev_layer3_weight = a.layer3_weight;
	a.prev_layer3_cycle = a.layer3_cycle;
	a.prev_layer6_weight = a.layer6_weight;
	a.prev_layer6_cycle = a.layer6_cycle;
	a.prev_layer7_weight = a.layer7_weight;
	a.prev_layer7_cycle = a.layer7_cycle;
	a.prev_layer12_weight = a.layer12_weight;
	a.prev_layer12_cycle = a.layer12_cycle;
	a.prev_feet_yaw = a.feet_yaw;
	a.prev_eye_yaw = a.eye_yaw;

	// ----- layers -----
	a.layer3_weight = layers[3].m_flWeight;
	a.layer3_cycle = layers[3].m_flCycle;
	a.layer6_weight = layers[6].m_flWeight;
	a.layer6_cycle = layers[6].m_flCycle;
	a.layer7_weight = layers[7].m_flWeight;
	a.layer7_cycle = layers[7].m_flCycle;
	a.layer12_weight = layers[12].m_flWeight;
	a.layer12_cycle = layers[12].m_flCycle;

	a.layer3_weight_delta = fabsf(a.layer3_weight - a.prev_layer3_weight);
	a.layer6_weight_delta = fabsf(a.layer6_weight - a.prev_layer6_weight);
	a.layer7_weight_delta = fabsf(a.layer7_weight - a.prev_layer7_weight);
	a.layer12_weight_delta = fabsf(a.layer12_weight - a.prev_layer12_weight);

	a.layer3_cycle_delta = cycle_delta(a.layer3_cycle, a.prev_layer3_cycle);
	a.layer6_cycle_delta = cycle_delta(a.layer6_cycle, a.prev_layer6_cycle);
	a.layer7_cycle_delta = cycle_delta(a.layer7_cycle, a.prev_layer7_cycle);
	a.layer12_cycle_delta = cycle_delta(a.layer12_cycle, a.prev_layer12_cycle);

	// ----- eye / feet -----
	const auto& animstate = record->m_state[resolver_direction::resolver_networked].m_animstate;

	a.eye_yaw = record->m_eye_angles.y;
	a.feet_yaw = animstate.foot_yaw;
	a.eye_feet_delta = math::normalize_float(a.eye_yaw - a.feet_yaw);

	if (previous)
		a.eye_yaw_delta = math::normalize_float(a.eye_yaw - previous->m_eye_angles.y);
	else
		a.eye_yaw_delta = math::normalize_float(a.eye_yaw - a.prev_eye_yaw);

	const float feet_snap = fabsf(math::normalize_float(a.feet_yaw - a.prev_feet_yaw));
	a.lby_snapped = (feet_snap > 20.f && feet_snap < 170.f);

	// ----- move state -----
	const Vector& vel = record->m_calculated_velocity.Length2D() > 1.f
		? record->m_calculated_velocity
		: record->m_velocity;

	a.speed_2d = vel.Length2D();
	a.on_ground = (record->m_flags & FL_ONGROUND) != 0;
	a.is_moving = a.speed_2d > 5.f;
	a.is_standing = !a.is_moving && a.on_ground;
	a.choke = record->m_lagamt;

	if (a.is_standing)
		++a.standing_ticks;
	else
		a.standing_ticks = 0;

	if (a.is_moving)
	{
		a.velocity_yaw = math::calc_angle(Vector{}, vel).y;
		a.velocity_yaw_delta = math::normalize_float(a.eye_yaw - a.velocity_yaw);
	}
	else
	{
		a.velocity_yaw = 0.f;
		a.velocity_yaw_delta = 0.f;
	}

	// ----- break flags -----
	a.layer7_strafe_break =
		a.layer7_weight > 0.50f ||
		a.layer7_weight_delta > 0.25f ||
		a.layer7_cycle_delta > 0.20f;

	a.layer6_break =
		a.layer6_weight_delta > 0.20f ||
		a.layer6_cycle_delta > 0.25f;

	a.layer3_break =
		a.layer3_weight_delta > 0.35f ||
		a.layer3_cycle_delta > 0.30f;

	// =========================================================================
	// Jitter / body-yaw sampling (+ bimodal tracking)
	// =========================================================================
	const bool whole_body_idle = a.layer12_weight < 0.25f;

	const bool l3_commit =
		a.layer3_weight > 0.01f &&
		(a.layer3_cycle_delta > 0.12f || a.layer3_weight_delta > 0.20f || a.layer3_break);

	const bool standing_desync_sample =
		a.is_standing &&
		a.standing_ticks >= 3 &&
		whole_body_idle &&
		fabsf(a.eye_feet_delta) > 12.f;

	const int cur_tick = interfaces::client_state()->get_last_server_tick();
	const bool weak_ok =
		standing_desync_sample &&
		(cur_tick - a.last_weak_sample_tick >= 2);

	const bool should_sample =
		a.is_standing &&
		whole_body_idle &&
		((l3_commit || a.lby_snapped) || weak_ok);

	if (should_sample)
	{
		if (weak_ok && !(l3_commit || a.lby_snapped))
			a.last_weak_sample_tick = cur_tick;

		const float sample = a.eye_yaw;

		if (j.sample_count == 0)
		{
			j.ewma = sample;
			j.ewm_var = 0.f;
			j.last_delta_sign = 0;
			j.sign_flip_count = 0;
			j.is_bimodal = false;
		}
		else
		{
			float delta = sample - j.ewma;
			while (delta > 180.f)  delta -= 360.f;
			while (delta < -180.f) delta += 360.f;

			const int cur_sign = (delta > 0.f) ? 1 : (delta < 0.f ? -1 : 0);
			if (cur_sign != 0 && j.last_delta_sign != 0)
			{
				if (cur_sign != j.last_delta_sign)
					j.sign_flip_count++;
				else
					j.sign_flip_count = std::max(0, j.sign_flip_count - 2);
			}
			if (cur_sign != 0)
				j.last_delta_sign = cur_sign;

			j.is_bimodal = j.sign_flip_count >= 3;

			j.ewma += j.ALPHA * delta;
			while (j.ewma > 180.f)  j.ewma -= 360.f;
			while (j.ewma < -180.f) j.ewma += 360.f;

			j.ewm_var = (1.f - j.ALPHA) * (j.ewm_var + j.ALPHA * delta * delta);
		}

		j.last_commit_yaw = sample;
		j.last_commit_tick = cur_tick;
		j.sample_count = std::min(j.sample_count + 1, 64);
		// confidence is computed only in yaw_resolve
	}
}

void resolver::yaw_resolve(const lag_record_t* record, const lag_record_t* previous)
{
	if (!record || record->m_shot || (previous && previous->m_shot))
		return;

	auto& log = player_log::get_log(record->m_index);
	const auto& a = log.m_anim;
	auto& j = log.m_jitter;

	// =========================================================================
	// 1. MODE
	// =========================================================================
	const auto previous_mode = log.m_current_mode;

	const bool big_eye_turn = fabsf(a.eye_yaw_delta) > 30.f;
	const bool huge_eye_turn = fabsf(a.eye_yaw_delta) > 170.f;
	const bool anim_break =
		a.layer7_strafe_break ||
		a.layer6_break ||
		a.layer3_break ||
		a.lby_snapped;

	const bool want_flip =
		(a.eye_yaw_delta > 30.f) ||
		(anim_break && a.eye_yaw_delta > 10.f);

	const bool want_nonflip =
		(a.eye_yaw_delta < -30.f) ||
		(anim_break && a.eye_yaw_delta < -10.f);

	if ((log.m_current_mode == resolver_mode::resolver_flip && want_nonflip) ||
		(log.m_current_mode == resolver_mode::resolver_default && want_flip))
	{
		log.m_current_mode = static_cast<resolver_mode>(!static_cast<int>(log.m_current_mode));
	}
	else if (huge_eye_turn || (anim_break && big_eye_turn))
	{
		log.m_current_mode = static_cast<resolver_mode>(!static_cast<int>(log.m_current_mode));
	}

	if (previous_mode != log.m_current_mode)
		log.m_last_flip_tick = interfaces::client_state()->get_last_server_tick();

	if (interfaces::client_state()->get_last_server_tick() - log.m_last_flip_tick > time_to_ticks(1.1f))
		log.m_current_mode = resolver_mode::resolver_default;

	// =========================================================================
	// 2. SIDE — wall > jitter (mono / bimodal) > eye_feet
	// =========================================================================
	bool side_set = false;

	// 2a. Wall
	if (log.m_wall_side_valid)
		side_set = true;

	// 2b. Jitter
	if (!side_set && j.sample_count >= j.MIN_SAMPLES)
	{
		const int ticks_since =
			interfaces::client_state()->get_last_server_tick() - j.last_commit_tick;

		const float recency_conf = 1.f - clamp(
			static_cast<float>(ticks_since) / static_cast<float>(j.COMMIT_FRESH_TICKS),
			0.f, 1.f);

		float body_yaw = j.ewma;
		bool  can_use = false;

		if (j.is_bimodal)
		{
			const float spread = sqrtf(std::max(j.ewm_var, 1.f));
			const float cluster_hi = math::normalize_float(j.ewma + spread);
			const float cluster_lo = math::normalize_float(j.ewma - spread);

			const float diff_hi = fabsf(math::angle_diff(record->m_eye_angles.y, cluster_hi));
			const float diff_lo = fabsf(math::angle_diff(record->m_eye_angles.y, cluster_lo));

			// Far cluster (aggressive; wrong on near-cluster ticks by design)
			body_yaw = (diff_hi > diff_lo) ? cluster_hi : cluster_lo;

			j.confidence = std::min(1.f, 0.45f + 0.1f * static_cast<float>(j.sign_flip_count))
				* std::max(recency_conf, 0.1f);

			can_use = recency_conf > 0.15f;
		}
		else
		{
			const float var_conf = 1.f - clamp(
				(j.ewm_var - j.VAR_LOW) / (j.VAR_HIGH - j.VAR_LOW),
				0.f, 1.f);

			j.confidence = var_conf * std::max(recency_conf, 0.1f);

			const float blend_t = clamp(
				static_cast<float>(ticks_since) / static_cast<float>(j.COMMIT_FRESH_TICKS),
				0.f, 1.f);
			body_yaw = math::lerp(j.last_commit_yaw, j.ewma, blend_t);

			can_use = j.confidence >= j.CONFIDENCE_GATE;
		}

		if (can_use)
		{
			const float offset = math::angle_diff(record->m_eye_angles.y, body_yaw);

			constexpr float DEADZONE = 5.f;
			if (fabsf(offset) >= DEADZONE)
			{
				log.m_current_side = (offset > 0.f)
					? resolver_side::resolver_right
					: resolver_side::resolver_left;
				side_set = true;
			}
		}
	}

	// 2c. Eye / feet
	if (!side_set)
	{
		float side_signal = a.eye_feet_delta;

		if (a.is_moving && a.speed_2d > 20.f)
			side_signal = a.eye_feet_delta * 0.55f + a.velocity_yaw_delta * 0.45f;

		if (fabsf(side_signal) >= 5.f)
		{
			log.m_current_side = (side_signal < 0.f)
				? resolver_side::resolver_left
				: resolver_side::resolver_right;
		}
		side_set = true;
	}

	// =========================================================================
	// 3. DIR
	// =========================================================================
	auto& slot = log.m_mode[log.m_current_mode].m_side[log.m_current_side];

	resolver_direction picked =
		(log.m_current_side == resolver_side::resolver_left)
		? resolver_direction::resolver_min
		: resolver_direction::resolver_max;

	if (a.is_standing && fabsf(a.eye_feet_delta) > 25.f)
	{
		picked = (a.eye_feet_delta < 0.f)
			? resolver_direction::resolver_min
			: resolver_direction::resolver_max;
	}

	if (slot.m_blacklist[picked])
	{
		const auto opposite =
			(picked == resolver_direction::resolver_min)
			? resolver_direction::resolver_max
			: resolver_direction::resolver_min;

		if (!slot.m_blacklist[opposite])
			picked = opposite;
	}

	slot.m_current_dir = picked;
}

void resolver::on_createmove()
{
	if ( tickbase::force_choke )
		return;

	std::vector<std::shared_ptr<detail::call_queue::queue_element>> calls;


	static Vector last_eyepos = {};
	const auto eyepos = local_player->get_eye_pos();

	for ( const auto player : interfaces::entity_list()->get_players() )
	{
		auto& log = player_log::get_log( player->EntIndex() );
		if ( player->IsDormant() || !player->is_enemy() || log.record.empty() || player->get_player_info().fakeplayer || !log.is_hittable )
			continue;

		auto& newest = log.record.back();

		if ( fabsf( eyepos.Length() - last_eyepos.Length() ) > 2.f )
			newest.m_did_wall_detect = false;

		if ( newest.m_did_wall_detect )
			continue;

		wall_detect( &newest );
	}

	last_eyepos = eyepos;
}

void resolver::wall_detect(lag_record_t* record)
{
	auto& log = player_log::get_log(record->m_index);
	const auto player = globals::get_player(record->m_index);
	if (!player)
		return;

	const auto weapon = local_weapon;
	if (!weapon || !weapon->is_gun())
		return;

	record->m_did_wall_detect = true;
	log.m_wall_side_valid = false;

	const Vector eye_pos = record->m_origin + Vector(0.f, 0.f, 64.f);
	const Vector target = current_eye;
	const float yaw = math::calc_angle(eye_pos, target).y;

	auto get_rotated = [](Vector start, float rotation, float dist) -> Vector
		{
			const float rad = DEG2RAD(rotation);
			start.x += cosf(rad) * dist;
			start.y += sinf(rad) * dist;
			return start;
		};

	const Vector local_left = get_rotated(eye_pos, math::normalize_float(yaw - 90.f), 18.f);
	const Vector local_right = get_rotated(eye_pos, math::normalize_float(yaw + 90.f), 18.f);

	auto get_damage = [&](const Vector& from, const Vector& to) -> float
		{
			aimbot::aimpoint_t point{};
			point.point = to;
			auto pen = *interfaces::weapon_system()->GetWpnData(WEAPON_AWP);
			pen.idamage = 200;
			can_hit(player, penetration::pen_data({}, {}, {}, {}, &pen), from, &point, point.damage);
			return static_cast<float>(point.damage);
		};

	const float dmg_left = get_damage(local_left, target);
	const float dmg_right = get_damage(local_right, target);

	resolver_side new_side = log.m_current_side;
	bool decisive = false;

	if (dmg_left > 0.f && dmg_right <= 0.f)
	{
		new_side = resolver_side::resolver_left;
		decisive = true;
	}
	else if (dmg_right > 0.f && dmg_left <= 0.f)
	{
		new_side = resolver_side::resolver_right;
		decisive = true;
	}
	else if (dmg_left > dmg_right * 1.35f)
	{
		new_side = resolver_side::resolver_left;
		decisive = true;
	}
	else if (dmg_right > dmg_left * 1.35f)
	{
		new_side = resolver_side::resolver_right;
		decisive = true;
	}

	if (!decisive)
		return;

	log.m_current_side = new_side;
	log.m_wall_side_valid = true;
	log.m_wall_detect_ang = math::normalize_float(
		yaw + (new_side == resolver_side::resolver_left ? -90.f : 90.f));
}

void resolver::add_shot( shot_t& shot )
{
	shots.emplace_back( shot );
}

void resolver::update_missed_shots( const ClientFrameStage_t& stage )
{
	if ( stage != FRAME_NET_UPDATE_END )
		return;

	auto it = shots.begin();
	while ( it != shots.end() )
	{
		const auto shot = *it;
		if ( shot.tick + time_to_ticks( 1.f ) < interfaces::globals()->tickcount || shot.tick - 10 > interfaces::globals()->tickcount )
		{
			it = shots.erase( it );
		}
		else
		{
			++it;
		}
	}

	auto it2 = current_shots.begin();
	while ( it2 != current_shots.end() )
	{
		const auto shot = *it2;
		if ( shot.tick + time_to_ticks( 1.f ) < interfaces::globals()->tickcount || shot.tick - 10 > interfaces::globals()->tickcount )
		{
			it2 = current_shots.erase( it2 );
		}
		else
		{
			++it2;
		}
	}
}

void resolver::hurt_listener( IGameEvent* game_event, record_shot_info_t& shot_info )
{
	const auto attacker = interfaces::engine()->GetPlayerForUserID( game_event->GetInt( "attacker" ) );
	const auto victim = interfaces::engine()->GetPlayerForUserID( game_event->GetInt( "userid"  ) );
	const auto hitgroup = game_event->GetInt( "hitgroup" );
	const auto damage = game_event->GetInt( "dmg_health" );

	if ( attacker != interfaces::engine()->GetLocalPlayer() )
		return;

	if ( victim == interfaces::engine()->GetLocalPlayer() )
		return;

	const auto player = globals::get_player( victim );
	if ( !player || !player->is_enemy() )
		return;

	if ( unapproved_shots.empty() )
		return;

	for ( auto& shot : unapproved_shots )
	{
		if ( !shot.hurt && shot.enemy_index == victim )
		{
			shot.hurt = true;
			shot.hitinfo.victim = victim;
			shot.hitinfo.hitgroup = hitgroup;
			shot.hitinfo.damage = damage;
			shot_info = shot.record.m_shot_info;
			return;
		}
	}
}

resolver::shot_t* resolver::closest_shot( int tickcount )
{
	shot_t* closest_shot = nullptr;
	for ( auto& shot : shots )
	{
		closest_shot = &shot;
		break;
	}

	return closest_shot;
}

bool resolver::record_shot( IGameEvent* game_event )
{
	const auto userid = interfaces::engine()->GetPlayerForUserID( game_event->GetInt( "userid" ) );
	const auto player = globals::get_player( userid );

	if ( player != local_player )
		return false;

	const auto shot = closest_shot( interfaces::globals()->tickcount - time_to_ticks( interfaces::engine()->GetNetChannelInfo()->GetLatency( FLOW_OUTGOING ) ) );
	if ( !shot )
		return false;

	current_shots.push_front( *shot );
	shots.pop_front();
	current_hitposes.clear();

	return true;
}

void resolver::listener( IGameEvent* game_event )
{
	static auto last_tickcount = 0;

	if ( !strcmp( game_event->GetName(), "weapon_fire" ) )
	{
		if ( record_shot( game_event ) )
			last_tickcount = 0;
		return;
	}

	if ( current_shots.empty() )
		return;

	const auto userid = interfaces::engine()->GetPlayerForUserID( game_event->GetInt( "userid"  ) );
	const auto player = globals::get_player( userid );

	if ( !player || player != local_player )
		return;

	const Vector pos( game_event->GetFloat( "x" ), game_event->GetFloat( "y" ), game_event->GetFloat( "z" ) );

	const auto shot = &current_shots[ 0 ];

	static auto counter = 0;

	if ( last_tickcount == interfaces::globals()->tickcount )
		counter++;
	else
	{
		current_hitposes.clear();
		counter = 0;
	}

	if ( counter )
		unapproved_shots.pop_front();

	current_hitposes.push_back( pos );
	shot->hitposes = current_hitposes;
	unapproved_shots.emplace_back( *shot );

	last_tickcount = interfaces::globals()->tickcount;
}

Vector resolver::get_closest_hitpos( const shot_t& shot, const Vector& pos )
{
	Vector closest = {};
	auto last_dist = FLT_MAX;
	for ( auto& hitpos : shot.hitposes )
	{
		const auto dist = hitpos.Dist( pos );
		if ( dist < last_dist )
		{
			last_dist = dist;
			closest = hitpos;
		}
	}

	return closest;
}

Vector resolver::get_closest_penetrationpos( const shot_t& shot, const Vector& pos )
{
	Vector closest = {};
	auto last_dist = FLT_MAX;
	for ( auto& hitpos : shot.penetration_points )
	{
		const auto dist = hitpos.Dist( pos );
		if ( dist < last_dist )
		{
			last_dist = dist;
			closest = hitpos;
		}
	}

	return closest;
}

void resolver::approve_shots( const ClientFrameStage_t& stage )
{
	if ( stage != FRAME_NET_UPDATE_END )
		return;

	for ( auto& shot : unapproved_shots )
	{
		if ( shot.hitposes.empty() )
			continue;

		auto end = shot.hitposes[ shot.hitposes.size() - 1 ];

		if ( vars::misc.impacts->get<bool>() )
		{
			auto col2 = Color( vars::misc.impacts_color2->get<D3DCOLOR>() );

			for ( auto& point : shot.hitposes )
				interfaces::debug_overlay()->AddBoxOverlay( point, Vector( -1.25f, -1.25f, -1.25f ), Vector( 1.25f, 1.25f, 1.25f ), QAngle( 0, 0, 0 ), col2.r(), col2.g(), col2.b(), 180, 4 );
		}

		if ( local_player && local_player->get_alive() && prediction::get_pred_info( shot.cmdnum ).sequence == shot.cmdnum )
		{
			auto new_origin = prediction::get_pred_info( shot.cmdnum ).origin;
			shot.shotpos.x = new_origin.x;
			shot.shotpos.y = new_origin.y;
		}

		const auto angles = math::calc_angle( shot.shotpos, end );
		Vector direction{};
		math::angle_vectors( angles, &direction );

		if ( shot.record.m_index == -1 )
		{
			if ( shot.hurt )
			{
				if ( shot.penetration_points.empty() )
					continue;

				shot.hitpos = get_closest_hitpos( shot, shot.penetration_points[ shot.penetration_points.size() - 1 ] );
			}

			Vector zerovec = {};
			lua::api.callback( FNV1A( "on_shot_registered" ), [&] ( lua::state& state )
			{
				state.create_table();
				state.set_field( XOR_STR( "manual" ), true );
				state.set_field( XOR_STR( "secure" ), false );
				state.set_field( XOR_STR( "very_secure" ), false );
				state.set_field( XOR_STR( "result" ), shot.hurt ? XOR_STR( "hit" ) : XOR_STR( "miss" ) );
				state.set_field( XOR_STR( "target" ), -1 );
				state.set_field( XOR_STR( "tick" ), shot.tick );
				state.set_field( XOR_STR( "backtrack" ), 0 );
				state.set_field( XOR_STR( "hitchance" ), -1 );
				state.set_field( XOR_STR( "client_hitgroup" ), -1 );
				state.set_field( XOR_STR( "client_damage" ), -1 );
				state.set_field( XOR_STR( "server_hitgroup" ), shot.hitinfo.hitgroup );
				state.set_field( XOR_STR( "server_damage" ), shot.hitinfo.damage );
				state.create_user_object<decltype( shot.shotpos )>( XOR_STR( "vec3" ), &shot.shotpos );
				state.set_field( XOR_STR( "shotpos" ) );
				state.create_user_object<decltype( zerovec )>( XOR_STR( "vec3" ), &zerovec );
				state.set_field( XOR_STR( "client_hitpos" ) );
				state.create_user_object<decltype( shot.hitpos )>( XOR_STR( "vec3" ), shot.hurt ? &shot.hitpos : &zerovec );
				state.set_field( XOR_STR( "server_hitpos" ) );
				state.create_table();
				{
					auto index = 1;
					for ( auto cur : shot.penetration_points )
					{
						state.create_user_object<decltype( cur )>( XOR_STR( "vec3" ), &cur );
						state.set_i( index++ );
					}
				}
				state.set_field( XOR_STR( "client_impacts" ) );
				state.create_table();
				{
					auto index = 1;
					for ( auto cur : shot.hitposes )
					{
						state.create_user_object<decltype( cur )>( XOR_STR( "vec3" ), &cur );
						state.set_i( index++ );
					}
				}
				state.set_field( XOR_STR( "server_impacts" ) );
				return 1;
			} );

			if ( shot.hurt )
			{
				const auto player = globals::get_player( shot.hitinfo.victim );
				if ( player )
				{
					add_hit( hitmarker::hitmarker_t( interfaces::globals()->realtime, shot.hitinfo.victim, shot.hitinfo.damage, shot.hitinfo.hitgroup, shot.hitpos ) );

					if ( vars::visuals.beams.local.enabled->get<bool>() && vars::visuals.beams.enabled->get<bool>() )
						add_local_beam( beams::impact_info_t( interfaces::globals()->curtime, shot.shotpos, end, interfaces::engine()->GetLocalPlayer(), Color( vars::visuals.beams.local.color->get<D3DCOLOR>() ) ) );
					continue;
				}
			}

			if ( vars::visuals.beams.local.enabled->get<bool>() && vars::visuals.beams.enabled->get<bool>() )
				add_local_beam( beams::impact_info_t( interfaces::globals()->curtime, shot.shotpos, end, interfaces::engine()->GetLocalPlayer(), Color( vars::visuals.beams.local.color->get<D3DCOLOR>() ) ) );
			continue;
		}

		auto hitpos = get_closest_hitpos( shot, shot.hitgroup != -1 ? shot.hitpos : shot.record.m_origin );

		auto player = globals::get_player( shot.enemy_index );
		if ( vars::visuals.chams.enemy.shot_record.type->get<int>() && player )
			chams::add_ghost( player, &shot.record );

		if ( !player )
		{
			// maybe add shot info

			shot.hitpos = hitpos;
			if ( shot.hurt )
			{
				add_hit( hitmarker::hitmarker_t( interfaces::globals()->realtime, shot.hitinfo.victim, shot.hitinfo.damage, shot.hitinfo.hitgroup, hitpos ) );

				if ( vars::visuals.beams.local.enabled->get<bool>() && vars::visuals.beams.enabled->get<bool>() && !beams::beam_exists( local_player, interfaces::globals()->curtime ) )
					add_local_beam( beams::impact_info_t( interfaces::globals()->curtime, shot.shotpos, end, interfaces::engine()->GetLocalPlayer(), Color( vars::visuals.beams.local.color->get<D3DCOLOR>() ) ) );
			}
			else if ( vars::visuals.beams.local.enabled->get<bool>() && vars::visuals.beams.enabled->get<bool>() )
				add_local_beam( beams::impact_info_t( interfaces::globals()->curtime, shot.shotpos, end, interfaces::engine()->GetLocalPlayer(), Color( vars::visuals.beams.local.color->get<D3DCOLOR>() ) ) );

			continue;
		}

		if ( !local_player || !local_player->get_alive() || !local_weapon )
			continue;

		shot.hitpos = shot.hitposes[ shot.hitposes.size() - 1 ] + direction * 1000.f;

		auto& log = player_log::get_log( shot.enemy_index );
		auto data = penetration::pen_data( &shot.record, shot.record.m_shot_dir, false, nullptr, &shot.weapon_data );

		if ( shot.record.m_shot_info.extrapolated && !log.record.empty() && !log.record.back().m_dormant )
		{
			//aimbot_helpers::draw_debug_hitboxes( player, log.record.back().matrix( shot.record.m_shot_state ), -1, 5.f, Color( 0, 255, 255, 255 ) );
			data = penetration::pen_data( &log.record.back(), shot.record.m_shot_dir, false, nullptr, &shot.weapon_data );
		}

		aimbot::aimpoint_t aimpoint;
		aimpoint.hitbox = -1;
		aimpoint.point = end;

		auto damage = 0;
		auto new_data = data;
		if ( can_hit( local_player, new_data, shot.shotpos, &aimpoint, damage, true ) )
		{
			hitpos = get_closest_hitpos( shot, aimpoint.point );
			shot.hitpos = hitpos;
			shot.hit = true;
			shot.hit_originally = true;
		}

		const auto deltavec = Vector( shot.original_shotpos.x - shot.shotpos.x, shot.original_shotpos.y - shot.shotpos.y, 0 );
		const auto corrected_pos = fabsf( deltavec.x ) >= 0.001f || fabsf( deltavec.y ) >= 0.001f;

		if ( corrected_pos )
		{
			auto damage2 = 0;
			shot.hit_originally = can_hit( local_player, data, shot.original_shotpos, &aimpoint, damage2, true );
		}

		if ( shot.record.m_shot_info.extrapolated )
		{
			//aimbot_helpers::draw_debug_hitboxes( player, shot.record.matrix( shot.record.m_shot_state ), -1, 5.f, Color( 255, 255, 255, 255 ) );

			auto damage2 = 0;
			shot.hit_extrapolation = can_hit( local_player, penetration::pen_data( &shot.record, shot.record.m_shot_dir, false, nullptr, &shot.weapon_data ), shot.shotpos, &aimpoint, damage2, true );
		}

		if ( vars::visuals.beams.local.enabled->get<bool>() && vars::visuals.beams.enabled->get<bool>() )
			add_local_beam( beams::impact_info_t( interfaces::globals()->curtime, shot.shotpos, end, interfaces::engine()->GetLocalPlayer(), Color( vars::visuals.beams.local.color->get<D3DCOLOR>() ) ) );

		if ( shot.hurt )
			add_hit( hitmarker::hitmarker_t( interfaces::globals()->realtime, shot.hitinfo.victim, shot.hitinfo.damage, shot.hitinfo.hitgroup, hitpos ) );

		if ( shot.hitgroup == -1 )
			continue;

		Vector zerovec = {};

		lua::api.callback( FNV1A( "on_shot_registered" ), [&] ( lua::state& state )
		{
			state.create_table();
			state.set_field( XOR_STR( "manual" ), shot.hitgroup == -1 );
			state.set_field( XOR_STR( "secure" ), shot.safety >= penetration::safety_no_roll );
			state.set_field( XOR_STR( "very_secure" ), shot.safety >= penetration::safety_full );
			state.set_field( XOR_STR( "result" ), shot.hurt ? XOR_STR( "hit" ) : shot.hit ? XOR_STR( "resolve" ) : shot.hit_extrapolation ? ( !ConVar::cl_lagcompensation || !ConVar::cl_predict ) ? XOR_STR( "anti-exploit" ) : XOR_STR( "extrapolation" ) : shot.hit_originally ? XOR_STR( "server correction" ) : XOR_STR( "spread" ) );
			state.set_field( XOR_STR( "target" ), shot.enemy_index );
			state.set_field( XOR_STR( "tick" ), shot.tick );
			state.set_field( XOR_STR( "backtrack" ), shot.record.m_shot_info.backtrack_ticks );
			state.set_field( XOR_STR( "hitchance" ), shot.record.m_shot_info.hitchance );
			state.set_field( XOR_STR( "client_hitgroup" ), shot.hitgroup );
			state.set_field( XOR_STR( "client_damage" ), shot.damage );
			state.set_field( XOR_STR( "server_hitgroup" ), shot.hitinfo.hitgroup );
			state.set_field( XOR_STR( "server_damage" ), shot.hitinfo.damage );
			state.create_user_object<decltype( shot.shotpos )>( XOR_STR( "vec3" ), &shot.shotpos );
			state.set_field( XOR_STR( "shotpos" ) );
			state.create_user_object<decltype( end )>( XOR_STR( "vec3" ), &end );
			state.set_field( XOR_STR( "client_hitpos" ) );
			state.create_user_object<decltype( shot.hitpos )>( XOR_STR( "vec3" ), shot.hurt ? &shot.hitpos : &zerovec );
			state.set_field( XOR_STR( "server_hitpos" ) );
			state.create_table();
			{
				auto index = 1;
				for ( auto cur : shot.penetration_points )
				{
					state.create_user_object<decltype( cur )>( XOR_STR( "vec3" ), &cur );
					state.set_i( index++ );
				}
			}
			state.set_field( XOR_STR( "client_impacts" ) );
			state.create_table();
			{
				auto index = 1;
				for ( auto cur : shot.hitposes )
				{
					state.create_user_object<decltype( cur )>( XOR_STR( "vec3" ), &cur );
					state.set_i( index++ );
				}
			}
			state.set_field( XOR_STR( "server_impacts" ) );
			return 1;
		} );

		if ( player->get_player_info().fakeplayer )
		{
			calc_missed_shots( &shot );

			continue;
		}

		if ( vars::legit_enabled() )
			continue;

		get_brute_angle( &shot );

		calc_missed_shots( &shot );
	}

	current_shots.clear();
	unapproved_shots.clear();
	current_hitposes.clear();
}

void resolver::get_brute_angle(shot_t* shot)
{
	if (!local_player || !local_player->get_alive() || !local_weapon || !shot || shot->record.m_dormant)
		return;

	const auto player = globals::get_player(shot->enemy_index);
	if (!player || !player->get_alive() || player->get_player_info().fakeplayer)
		return;

	if (vars::legit_enabled())
		return;

	auto& log = player_log::get_log(shot->enemy_index);

	const auto mode = shot->record.m_shot
		? resolver_mode::resolver_shot
		: shot->record.m_resolver_mode;
	const auto side = shot->record.m_resolver_side;
	auto& slot = log.m_mode[mode].m_side[side];

	const auto tried = shot->record.m_shot_dir;

	const bool resolve_miss = shot->hit && !shot->hurt;
	const bool registered_hit = shot->hurt;

	// Spread / no registration — leave anim resolve alone
	if (!resolve_miss && !registered_hit)
		return;

	auto is_min_family = [](resolver_direction d) -> bool
		{
			return d == resolver_direction::resolver_min
				|| d == resolver_direction::resolver_min_extra
				|| d == resolver_direction::resolver_min_min;
		};

	auto is_max_family = [](resolver_direction d) -> bool
		{
			return d == resolver_direction::resolver_max
				|| d == resolver_direction::resolver_max_extra
				|| d == resolver_direction::resolver_max_max;
		};

	auto pick_opposite_primary = [&](resolver_direction from) -> resolver_direction
		{
			// Prefer plain min/max; only step to extras if primaries are blocked
			const resolver_direction primary =
				is_min_family(from) ? resolver_direction::resolver_max
				: is_max_family(from) ? resolver_direction::resolver_min
				: (side == resolver_side::resolver_left
					? resolver_direction::resolver_max
					: resolver_direction::resolver_min);

			if (!slot.m_blacklist[primary])
				return primary;

			const resolver_direction extra =
				(primary == resolver_direction::resolver_max)
				? resolver_direction::resolver_max_extra
				: resolver_direction::resolver_min_extra;

			if (!slot.m_blacklist[extra]
				&& (!vars::aim.resolver_mode->get<int>() || extra < resolver_direction::resolver_max_extra))
				return extra;

			// Last resort: anything not blacklisted in the opposite family
			const resolver_direction begin =
				(primary == resolver_direction::resolver_max)
				? resolver_direction::resolver_max
				: resolver_direction::resolver_min;
			const resolver_direction end =
				(primary == resolver_direction::resolver_max)
				? resolver_direction::resolver_max_max
				: resolver_direction::resolver_min_extra;

			for (auto d = begin; d <= end; d = static_cast<resolver_direction>(static_cast<int>(d) + 1))
			{
				if (!slot.m_blacklist[d])
					return d;
			}

			return primary; // give up — anim will keep trying
		};

	// -------------------------------------------------------------------------
	// Resolve miss: blacklist what we shot, flip to opposite primary
	// -------------------------------------------------------------------------
	if (resolve_miss)
	{
		if (tried > resolver_direction::resolver_networked)
			slot.m_blacklist[tried] = true;

		slot.m_current_dir = pick_opposite_primary(tried);

		// Mirror a simple opposite onto the other mode so flip bucket isn't stale
		if (mode != resolver_mode::resolver_shot && log.m_unknown)
		{
			const auto other = static_cast<resolver_mode>((static_cast<int>(mode) + 1) % 2);
			auto& other_slot = log.m_mode[other].m_side[side];
			if (!other_slot.m_blacklist[slot.m_current_dir])
				other_slot.m_current_dir = slot.m_current_dir;
		}
	}

	// -------------------------------------------------------------------------
	// Hit: lock the dir that worked, clear its blacklist
	// -------------------------------------------------------------------------
	if (registered_hit)
	{
		if (tried > resolver_direction::resolver_networked)
		{
			slot.m_blacklist[tried] = false;
			slot.m_current_dir = tried;
		}
	}

	// -------------------------------------------------------------------------
	// Unknown flags
	// -------------------------------------------------------------------------
	if (slot.m_current_dir != tried || registered_hit)
	{
		if (mode == resolver_mode::resolver_shot)
			log.m_unknown_shot = false;
		else
			log.m_unknown = false;
	}
}

void resolver::calc_missed_shots(shot_t* shot)
{
	if (!shot)
		return;

	auto& log = player_log::get_log(shot->enemy_index);

	const auto dir_name = [](resolver_direction d) -> const char*
		{
			switch (d)
			{
			case resolver_direction::resolver_networked: return "networked";
			case resolver_direction::resolver_max:        return "max";
			case resolver_direction::resolver_zero:       return "zero";
			case resolver_direction::resolver_min:        return "min";
			case resolver_direction::resolver_max_extra:  return "max_extra";
			case resolver_direction::resolver_max_max:    return "max_max";
			case resolver_direction::resolver_min_min:    return "min_min";
			case resolver_direction::resolver_min_extra:  return "min_extra";
			default:                                      return "unknown";
			}
		};

	const auto side_name = [](resolver_side s) -> const char*
		{
			switch (s)
			{
			case resolver_side::resolver_left:  return "left";
			case resolver_side::resolver_right: return "right";
			default:                            return "invalid";
			}
		};

	const auto mode_name = [](resolver_mode m) -> const char*
		{
			switch (m)
			{
			case resolver_mode::resolver_default: return "default";
			case resolver_mode::resolver_flip:    return "flip";
			case resolver_mode::resolver_shot:    return "shot";
			default:                              return "invalid";
			}
		};

	const auto dir = shot->record.m_shot_dir;
	const auto side = shot->record.m_resolver_side;
	const auto mode = shot->record.m_resolver_mode;

	// ----- Dormant -----
	if (shot->record.m_dormant)
	{
		if (shot->hurt)
		{
			const auto sound_player = sound_esp::get_sound_player(shot->enemy_index);
			sound_player->last_update_tick = interfaces::client_state()->get_last_server_tick();
			sound_player->updated = true;
			log.m_dormant_misses = 0;
		}
		else
		{
			log.m_dormant_misses++;
			interfaces::cvar()->ConsoleColorPrintf(Color(235, 5, 90), xorstr_("[fatality] "));
			util::print_dev_console(true, Color(255, 100, 100),
				xorstr_("missed dormant (total: %d)\n"), log.m_dormant_misses);
		}
		return;
	}

	if (shot->hurt && globals::nospread && shot->hitinfo.hitgroup == HITGROUP_HEAD && !shot->record.m_shot)
		log.nospread.m_pitch_cycle = 0;

	interfaces::cvar()->ConsoleColorPrintf(Color(235, 5, 90), xorstr_("[fatality] "));

	// ----- Hit -----
	if (shot->hurt)
	{
		util::print_dev_console(true, Color(100, 255, 100),
			xorstr_("hit - dir=%-10s side=%-5s mode=%-7s safety=%d dmg=%d hc=%.0f jitter_n=%d conf=%.2f\n"),
			dir_name(dir),
			side_name(side),
			mode_name(mode),
			shot->safety,
			shot->hitinfo.damage > 0 ? shot->hitinfo.damage : shot->damage,
			shot->hitchance,
			log.m_jitter.sample_count,
			log.m_jitter.confidence);
		return;
	}

	// ----- Client hit, server miss (resolve) -----
	if (shot->hit)
	{
		if (shot->record.m_unknown)
			log.m_unknown_misses++;
		log.m_shots++;

		util::print_dev_console(true, Color(255, 80, 80),
			xorstr_("miss resolve - dir=%-10s side=%-5s mode=%-7s safety=%d dmg=%d hc=%.0f jitter_n=%d conf=%.2f\n"),
			dir_name(dir),
			side_name(side),
			mode_name(mode),
			shot->safety,
			shot->damage,
			shot->hitchance,
			log.m_jitter.sample_count,
			log.m_jitter.confidence);
		return;
	}

	// ----- No client registration -----
	log.m_shots_spread++;

	const char* reason = "spread";
	if (shot->hit_extrapolation)
		reason = (!ConVar::cl_lagcompensation || !ConVar::cl_predict) ? "anti-exploit" : "extrapolation";
	else if (shot->hit_originally)
		reason = "server correction";

	util::print_dev_console(true, Color(255, 180, 80),
		xorstr_("miss %s - dir=%-10s side=%-5s mode=%-7s safety=%d dmg=%d hc=%.0f\n"),
		reason,
		dir_name(dir),
		side_name(side),
		mode_name(mode),
		shot->safety,
		shot->damage,
		shot->hitchance);
}

void resolver::set_local_info()
{
	last_origin_diff = local_player->get_origin() - last_origin;
	last_eye = local_player->get_eye_pos();
	last_origin = local_player->get_origin();
	current_eye = local_player->get_eye_pos();
}
