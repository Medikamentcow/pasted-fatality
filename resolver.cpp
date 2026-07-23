#include <random>
#include "../include_cheat.h"

void resolver::resolve(C_CSPlayer* player, lag_record_t* record, lag_record_t* previous)
{
	if (!player->is_enemy())
		return;

	if (!record->m_shot)
		pitch_resolve(record);

	auto& log = player_log::get_log(player->EntIndex());

	update_animation_features(log, record);
	update_kalman_from_anim(log);

	if (!player->get_player_info().fakeplayer)
		yaw_resolve(record, previous);
}

void resolver::post_animate(C_CSPlayer* player, lag_record_t* record)
{
	auto& log = player_log::get_log(player->EntIndex());

	if (vars::aim.resolver_mode->get<int>())
	{
		for (auto& mode : log.m_mode)
		{
			for (auto& side : mode.m_side)
			{
				if (side.m_current_dir < resolver_direction::resolver_networked ||
					side.m_current_dir >= resolver_direction::resolver_direction_max)
				{
					side.m_current_dir = resolver_direction::resolver_networked;
					log.m_unknown_shot = true;
					log.m_unknown = true;
				}
			}
		}
	}

	if (!player->is_enemy() || player->get_player_info().fakeplayer)
	{
		log.m_mode[resolver_mode::resolver_shot].m_side =
			log.m_mode[resolver_mode::resolver_default].m_side =
			log.m_mode[resolver_mode::resolver_flip].m_side = {};
	}

	record->m_resolver_mode = record->m_shot ? resolver_mode::resolver_shot : log.m_current_mode;
	record->m_resolver_side = log.m_current_side;

	if (!record->m_shot)
	{
		const auto cureye = record->m_eye_angles;
		if (fabsf(cureye.x) >= 60.f)
			log.m_last_unusual_pitch = interfaces::globals()->curtime;
		else
			log.m_last_zero_pitch = interfaces::globals()->curtime;
	}

	if (log.m_unknown_shot && log.m_mode[log.m_current_mode].m_side[log.m_current_side].m_current_dir > resolver_direction::resolver_networked)
	{
		log.m_mode[resolver_mode::resolver_shot].m_side[log.m_current_side].m_current_dir =
			log.m_mode[log.m_current_mode].m_side[log.m_current_side].m_current_dir;
	}

	if (!record->m_shot && player->is_enemy() && !player->get_player_info().fakeplayer)
	{
		const auto mode_to_write = record->m_resolver_mode;

		if (log.m_kalman.variance > 0.35f)
		{
			if (log.m_current_side == resolver_side::resolver_left)
				update_kalman(log, -0.35f, 0.55f);
			else if (log.m_current_side == resolver_side::resolver_right)
				update_kalman(log, 0.35f, 0.55f);
		}

		if (record->m_extrapolated)
		{
			const float extra = record->m_extrapolate_amt > 0
				? 0.04f * static_cast<float>(record->m_extrapolate_amt)
				: 0.06f;

			log.m_kalman.variance = std::min(log.m_kalman.variance + extra, 0.50f);
		}
	}
}

bool resolver::extrapolate_record(int ticks, lag_record_t& outrecord, const bool simple)
{
	if (!ticks)
	{
		outrecord.setup_matrices();
		return true;
	}

	const auto player = globals::get_player(outrecord.m_index);
	if (!player)
		return false;

	// -------------------- backups --------------------
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

	auto& log = player_log::get_log(outrecord.m_index);

	//simple (misleading)
	if (simple)
	{
		process_move_changes_t backup_pm{};
		backup_pm.store(player);

		const auto& original_record = log.record.back();
		const lag_record_t* p1 = log.record.size() > 1 ? &log.record[log.record.size() - 2] : nullptr;
		const lag_record_t* p2 = log.record.size() > 2 ? &log.record[log.record.size() - 3] : nullptr;

		Vector predicted_vel_change{};
		if (p1 && p2)
		{
			const Vector change1 = (p1->m_calculated_velocity - p2->m_calculated_velocity) / std::max(1, p1->m_lagamt);
			const Vector change2 = (original_record.m_calculated_velocity - p1->m_calculated_velocity) / std::max(1, original_record.m_lagamt);
			predicted_vel_change = change2 + (change2 - change1) * 0.5f;
		}
		else if (p1)
		{
			predicted_vel_change = (original_record.m_calculated_velocity - p1->m_calculated_velocity) / std::max(1, original_record.m_lagamt);
		}

		if (predicted_vel_change.Length2D() > 35.f)
			predicted_vel_change = predicted_vel_change.Normalized() * 35.f;

		const float speed = original_record.m_velocity.Length2D();
		int prev_buttons = 0;

		CUserCmd cmd{};

		for (int i = 0; i < ticks; i++)
		{
			Vector desired_vel = player->get_velocity() + predicted_vel_change;
			QAngle move_ang;
			math::vector_angles(desired_vel, move_ang);

			cmd.viewangles.y = move_ang.y;
			cmd.viewangles.x = 0.f;

			cmd.forwardmove = speed > 5.f ? 450.f : (i % 2 ? 1.01f : -1.01f);
			cmd.sidemove = 0.f;

			if (original_record.m_duckamt > 0.f)
				cmd.buttons |= IN_DUCK;
			else
				cmd.buttons &= ~IN_DUCK;

			if (i == 0)
			{
				player->get_ducking() = player->get_duck_amt() > 0.f;
				player->get_ducked() = player->get_duck_amt() == 1.f;
				if (player->get_ducked())
					player->get_ducking() = false;

				prev_buttons = cmd.buttons;
				if (!(player->get_flags() & FL_ONGROUND))
					prev_buttons |= IN_JUMP;
			}

			if (!(player->get_flags() & FL_ONGROUND))
			{
				QAngle vel_ang;
				math::vector_angles(player->get_velocity(), vel_ang);

				const float delta = math::normalize_float(vel_ang.y - move_ang.y);
				if (fabsf(delta) > 20.f)
				{
					cmd.forwardmove = 0.f;
					cmd.sidemove = delta > 0.f ? 450.f : -450.f;
				}
			}

			else if (p1)
			{
				const float prev_speed = p1->m_velocity.Length2D();
				const float target_speed = (player->get_velocity() + predicted_vel_change).Length2D();

				if (speed < prev_speed - 5.f * original_record.m_lagamt || speed < 5.f)
				{
					CMoveData data = interfaces::game_movement()->setup_move(player, &cmd);
					aimbot_helpers::stop_to_speed(1.01f, &data, player);
					cmd.forwardmove = data.m_flForwardMove;
					cmd.sidemove = data.m_flSideMove;
				}
				else if (speed < prev_speed + 5.f * original_record.m_lagamt && speed > 5.f)
				{
					CMoveData data = interfaces::game_movement()->setup_move(player, &cmd);
					aimbot_helpers::stop_to_speed(target_speed, &data, player);
					cmd.forwardmove = data.m_flForwardMove;
					cmd.sidemove = data.m_flSideMove;
				}
			}

			CMoveData data = interfaces::game_movement()->setup_move(player, &cmd);
			data.m_nOldButtons = prev_buttons;
			const auto ret = interfaces::game_movement()->process_movement(player, &data);
			prev_buttons = data.m_nButtons;
			ret.restore(player);

			if (p1)
			{
				if (!(p1->m_flags & FL_ONGROUND) && !(original_record.m_flags & FL_ONGROUND) && (player->get_flags() & FL_ONGROUND))
					cmd.buttons |= IN_JUMP;
				else
					cmd.buttons &= ~IN_JUMP;
			}

			player->set_abs_origin(data.m_vecAbsOrigin);
			player->get_velocity() = data.m_vecVelocity;

			if (i == ticks - 1)
				outrecord.m_origin = data.m_vecAbsOrigin;
		}

		// Restore
		backup_pm.restore(player);
		player->set_abs_origin(backup_abs_origin);
		player->get_flags() = backup_flags;
		player->get_ground_entity() = backup_groundentity;
		player->get_move_type() = backup_move_type;
		player->get_velocity() = backup_velocity;
		player->get_ducking() = backup_ducking;

		return true;
	}
	
	//real simple
	new_previous->m_velocity = outrecord.m_calculated_velocity;
	outrecord.m_simtime += interfaces::globals()->interval_per_tick * ticks;
	outrecord.m_lagamt = ticks;

	animations::update_player_animations(&outrecord, player, new_previous.get());

	// Restore player state
	player->get_lby() = backup_lby;
	player->get_anim_layers() = backup_layers;
	*player->get_anim_state() = backup_state;
	player->get_pose_params() = backup_poses;
	player->set_abs_angles(backup_angle);
	player->get_velocity() = backup_velocity;

	for (auto& state : outrecord.m_state)
		state.m_setup_tick = -1;

	outrecord.setup_matrices(resolver_direction::resolver_invalid, true);

	return true;
}

void resolver::pitch_resolve(lag_record_t* record)
{
	const auto& log = player_log::get_log(record->m_index);

	if (globals::nospread)
	{
		if (log.nospread.m_pitch_cycle % 2 && log.nospread.m_can_fake)
		{
			record->m_eye_angles.x = -record->m_eye_angles.x;
		}
	}

	record->m_pitch_cycle = log.nospread.m_pitch_cycle;
}


float resolver::get_resolver_angle(const lag_record_t& record, resolver_direction direction, float eye_angle)
{
	switch (direction)
	{
	case resolver_direction::resolver_max:
		return math::normalize_float(eye_angle + record.m_state[direction].m_animstate.aim_yaw_max * record.m_yaw_modifier * 2.f);
	case resolver_direction::resolver_min:
		return math::normalize_float(eye_angle + record.m_state[direction].m_animstate.aim_yaw_min * record.m_yaw_modifier * 2.f);
	default:
		return eye_angle;
	}
}

void resolver::update_animation_features(player_log_t& log, lag_record_t* record)
{
	if (!record)
		return;

	auto& anim = log.m_anim;

	// ----- Animation layers -----
	const auto& layers = record->m_layers;
	anim.layer3_weight = layers[3].m_flWeight;
	anim.layer3_cycle = layers[3].m_flCycle;
	anim.layer6_weight = layers[6].m_flWeight;
	anim.layer6_cycle = layers[6].m_flCycle;
	anim.layer12_weight = layers[12].m_flWeight;
	anim.layer12_cycle = layers[12].m_flCycle;

	// ----- Yaws -----
	const auto& animstate = record->m_state[resolver_direction::resolver_networked].m_animstate;
	anim.eye_yaw = record->m_eye_angles.y;

	anim.prev_feet_yaw = anim.feet_yaw;
	anim.feet_yaw = animstate.foot_yaw;
	anim.eye_feet_delta = math::normalize_float(anim.eye_yaw - anim.feet_yaw);

	const float feet_snap_delta = fabsf(math::normalize_float(anim.feet_yaw - anim.prev_feet_yaw));
	anim.lby_snapped = (feet_snap_delta > 20.f && feet_snap_delta < 170.f);

	// ----- Movement state -----
	const Vector& vel = record->m_calculated_velocity.Length2D() > 1.f
		? record->m_calculated_velocity
		: record->m_velocity;
	const float speed_2d = vel.Length2D();

	anim.is_moving = speed_2d > 5.f;
	anim.is_standing = !anim.is_moving && (record->m_flags & FL_ONGROUND);
	anim.speed_2d = speed_2d;

	if (anim.is_standing)
		++anim.standing_ticks;
	else
		anim.standing_ticks = 0;

	if (anim.is_moving)
	{
		anim.velocity_yaw = math::calc_angle(Vector{}, vel).y;
		anim.velocity_yaw_delta = math::normalize_float(anim.eye_yaw - anim.velocity_yaw);
	}
	else
	{
		anim.velocity_yaw = 0.f;
		anim.velocity_yaw_delta = 0.f;
	}

	anim.adjust_weight = record->addon.adjust_weight;
	anim.choke_count = record->m_lagamt;
}


void resolver::update_kalman(player_log_t& log, float measurement, float measurement_noise)
{
	auto& k = log.m_kalman;
	auto& anim = log.m_anim;

	// ---------- Sign-conflict resistance ----------
	const bool sign_conflict =
		(measurement > 0.10f && k.bias < -0.10f) ||
		(measurement < -0.10f && k.bias > 0.10f);

	if (sign_conflict && k.variance < 0.20f)
	{
		const float commitment = 1.f - std::clamp(k.variance / 0.20f, 0.f, 1.f);
		measurement_noise += 0.50f * commitment;
	}

	// ---------- Adaptive process noise ----------
	float process_noise = 0.028f;

	if (anim.layer6_weight > 0.60f || fabsf(anim.eye_feet_delta) > 45.f)
		process_noise = 0.055f;

	if (anim.is_moving)
		process_noise = std::max(process_noise, 0.040f);

	if (anim.is_standing && anim.standing_ticks > 8 && fabsf(anim.eye_feet_delta) < 25.f)
		process_noise = 0.018f;

	// Agreeing measurement + low variance → stickier
	if (k.variance < 0.08f && fabsf(k.bias) > 0.15f &&
		((measurement > 0.f) == (k.bias > 0.f)))
		process_noise = std::max(process_noise * 0.60f, 0.010f);

	// ---------- Bias trend (per-player) ----------
	if ((k.bias > 0.f && k.bias > anim.prev_bias + 0.01f) ||
		(k.bias < 0.f && k.bias < anim.prev_bias - 0.01f))
	{
		anim.bias_trend_counter++;
	}
	else if (fabsf(k.bias - anim.prev_bias) < 0.005f)
	{
		anim.bias_trend_counter = std::max(anim.bias_trend_counter - 1, 0);
	}
	else
	{
		anim.bias_trend_counter = 0;
	}
	anim.prev_bias = k.bias;

	// Consistent trend + strong bias → slightly more confident
	if (anim.bias_trend_counter > 8 && fabsf(k.bias) > 0.60f)
		process_noise *= 0.75f;

	// LBY snap → allow faster adaptation
	if (anim.lby_snapped)
		process_noise = std::max(process_noise, 0.070f);

	k.variance += process_noise;
	k.variance = std::max(k.variance, 0.004f);

	// ---------- Kalman update ----------
	const float innovation = measurement - k.bias;
	const float innovation_var = k.variance + measurement_noise;
	const float gain = std::min(k.variance / innovation_var, 0.78f);

	k.bias += gain * innovation;
	k.variance *= (1.f - gain);

	// Gentle decay on very weak measurements
	if (measurement_noise > 0.60f)
		k.bias *= 0.993f;

	k.bias = std::clamp(k.bias, -1.f, 1.f);
}

void resolver::update_kalman_from_anim(player_log_t& log)
{
	auto& anim = log.m_anim;
	auto& k = log.m_kalman;

	float measurement = 0.f;
	float noise = 0.90f;

	if (anim.is_standing && anim.standing_ticks > 2)
	{
		measurement = std::clamp(anim.eye_feet_delta / 58.f, -1.f, 1.f);
		noise = 0.25f;

		if (anim.layer6_weight > 0.55f)
			noise = 0.18f;

		if (anim.layer6_weight > 0.55f && anim.layer12_weight > 0.40f)
			noise = 0.13f;

		// Aggressive standing lock-in
		if (anim.standing_ticks > 16 && fabsf(anim.eye_feet_delta) > 18.f)
			noise = std::max(noise - 0.06f, 0.07f);

		if (anim.standing_ticks > 30 && fabsf(anim.eye_feet_delta) > 22.f)
			noise = std::max(noise - 0.03f, 0.05f);

		// Layer 6 jitter protection (per-player)
		const float layer6_delta = fabsf(anim.layer6_weight - anim.prev_layer6_weight);
		anim.prev_layer6_weight = anim.layer6_weight;

		if (layer6_delta > 0.12f)
			noise = std::min(noise + 0.14f, 0.55f);
		else if (layer6_delta > 0.06f)
			noise = std::min(noise + 0.07f, 0.40f);

		// Layer 12 trust — tighten noise only (no direct bias write)
		if (anim.layer12_weight > 0.65f && anim.standing_ticks > 15)
			noise *= 0.80f;

		// Layer 6 locked + strong desync — trust measurement more
		if (layer6_delta < 0.03f && fabsf(measurement) > 0.65f)
			noise = std::max(noise * 0.85f, 0.06f);

		// Layer 3 snap — AA likely flipped, loosen filter
		const float layer3_delta = fabsf(anim.layer3_weight - anim.prev_layer3_weight);
		anim.prev_layer3_weight = anim.layer3_weight;

		if (layer3_delta > 0.40f && anim.standing_ticks > 6)
		{
			k.variance *= 1.5f;
			k.variance = std::max(k.variance, 0.08f);
		}

		// Long idle + high layer12 — max confidence window
		if (anim.standing_ticks > 20 && anim.layer12_weight > 0.70f)
		{
			k.variance = std::min(k.variance, 0.12f);
			noise = std::min(noise, 0.08f);
		}

		// LBY snap — stronger trust in the new feet direction briefly
		if (anim.lby_snapped)
			noise = std::min(noise, 0.12f);
	}
	else if (anim.is_moving)
	{
		measurement = std::clamp(anim.velocity_yaw_delta / 65.f, -1.f, 1.f);

		// Speed-scaled noise (fixes slow-move hesitation)
		if (anim.speed_2d < 40.f)
			noise = 0.35f;
		else if (anim.speed_2d < 90.f)
			noise = 0.48f;
		else
			noise = 0.58f;
	}
	else
	{
		measurement = std::clamp(anim.eye_feet_delta / 58.f, -1.f, 1.f);
		noise = 0.90f;
	}

	update_kalman(log, measurement, noise);
}

resolver_direction resolver::bias_to_direction(float bias)
{
	const float hysteresis = 0.10f;

	if (bias < -0.55f + hysteresis) return resolver_direction::resolver_min_extra;
	if (bias < -0.32f + hysteresis) return resolver_direction::resolver_min;

	if (bias > 0.55f - hysteresis) return resolver_direction::resolver_max_extra;
	if (bias > 0.32f - hysteresis) return resolver_direction::resolver_max;

	if (fabsf(bias) < 0.20f) return resolver_direction::resolver_zero;
	return resolver_direction::resolver_networked;
}

void resolver::yaw_resolve(const lag_record_t* record, const lag_record_t* previous)
{
	if (!record || record->m_shot || (previous && previous->m_shot))
		return;

	auto& log = player_log::get_log(record->m_index);
	const auto& anim = log.m_anim;

	float layer6_delta = 0.f;
	float layer3_delta = 0.f;
	float feet_delta_change = 0.f;

	if (previous)
	{
		layer6_delta = fabsf(record->m_layers[6].m_flWeight - previous->m_layers[6].m_flWeight);
		layer3_delta = fabsf(record->m_layers[3].m_flWeight - previous->m_layers[3].m_flWeight);

		const float prev_feet = previous->m_state[resolver_direction::resolver_networked].m_animstate.foot_yaw;
		const float prev_eye_feet = math::normalize_float(previous->m_eye_angles.y - prev_feet);
		feet_delta_change = fabsf(anim.eye_feet_delta - prev_eye_feet);
	}

	const bool standing = anim.is_standing && anim.standing_ticks > 1;
	const bool strong_layer6 = standing && layer6_delta > 0.45f;
	const bool strong_layer3 = standing && layer3_delta > 0.40f;
	const bool strong_feet = !anim.is_moving && feet_delta_change > 40.f;
	const bool extreme_desync = fabsf(anim.eye_feet_delta) > 105.f;
	const bool lby_flip = anim.lby_snapped;

	const bool pattern_break = strong_layer6 || strong_layer3 || strong_feet || extreme_desync || lby_flip;
	if (!pattern_break)
	{
		// Idle: eventually fall back to default mode bucket (blacklist hygiene only)
		if (interfaces::client_state()->get_last_server_tick() - log.m_last_flip_tick > time_to_ticks(1.1f))
			log.m_current_mode = resolver_mode::resolver_default;
		return;
	}

	// ----- Volatility signal for Kalman (this is the point of mode-flip now) -----
	log.m_kalman.variance = std::min(log.m_kalman.variance + 0.18f, 0.55f);
	log.m_kalman.bias *= 0.85f; // allow side to move after AA flip

	// Mode toggle is optional bookkeeping for per-mode blacklist only — does NOT pick shot dir
	const float confidence = 1.f - std::clamp(log.m_kalman.variance, 0.f, 1.f);
	if (confidence < 0.70f || fabsf(log.m_kalman.bias) < 0.40f)
	{
		const auto previous_mode = log.m_current_mode;
		log.m_current_mode = static_cast<resolver_mode>(!static_cast<int>(log.m_current_mode));

		if (previous_mode != log.m_current_mode)
			log.m_last_flip_tick = interfaces::client_state()->get_last_server_tick();
	}
	else
	{
		// Confident filter: still opened variance above, but don't thrash mode buckets
		log.m_last_flip_tick = interfaces::client_state()->get_last_server_tick();
	}
}

void resolver::on_createmove()
{
	if (tickbase::force_choke)
		return;

	std::vector<std::shared_ptr<detail::call_queue::queue_element>> calls;


	static Vector last_eyepos = {};
	const auto eyepos = local_player->get_eye_pos();

	for (const auto player : interfaces::entity_list()->get_players())
	{
		auto& log = player_log::get_log(player->EntIndex());
		if (player->IsDormant() || !player->is_enemy() || log.record.empty() || player->get_player_info().fakeplayer || !log.is_hittable)
			continue;

		auto& newest = log.record.back();

		if (fabsf(eyepos.Length() - last_eyepos.Length()) > 2.f)
			newest.m_did_wall_detect = false;

		if (newest.m_did_wall_detect)
			continue;

		wall_detect(&newest);
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

	// ---------- Lightweight freestand ----------
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
			return point.damage;
		};

	const float dmg_left = get_damage(local_left, target);
	const float dmg_right = get_damage(local_right, target);

	// Decide side
	resolver_side new_side = log.m_current_side;
	if (dmg_left > 0.f && dmg_right <= 0.f)
		new_side = resolver_side::resolver_left;
	else if (dmg_right > 0.f && dmg_left <= 0.f)
		new_side = resolver_side::resolver_right;
	else if (dmg_left > dmg_right * 1.35f)
		new_side = resolver_side::resolver_left;
	else if (dmg_right > dmg_left * 1.35f)
		new_side = resolver_side::resolver_right;

	log.m_current_side = new_side;
	log.m_wall_detect_ang = math::normalize_float(yaw + (new_side == resolver_side::resolver_left ? -90.f : 90.f));

	// ---------- Proactive Kalman measurement ----------
	float freestand_meas = 0.f;
	float noise = 0.55f;

	if (dmg_left > 0.f || dmg_right > 0.f)
	{
		const float total = dmg_left + dmg_right + 1.f;
		freestand_meas = (dmg_right - dmg_left) / total;
		noise = 0.35f;

		// ============ HEURISTIC: Asymmetric damage = strong bias signal ============
		// If one side is heavily favored, nudge bias hard toward it
		const float damage_ratio = std::max(dmg_left, dmg_right) / std::max(1.f, std::min(dmg_left, dmg_right));

		if (damage_ratio > 2.5f)  // Strong asymmetry
		{
			noise *= 0.75f;  // Increase trust
			const float strong_nudge = freestand_meas * 0.15f;
			log.m_kalman.bias += strong_nudge;
		}
		else if (damage_ratio > 1.5f)  // Moderate asymmetry
		{
			noise *= 0.88f;
			const float moderate_nudge = freestand_meas * 0.08f;
			log.m_kalman.bias += moderate_nudge;
		}
	}

	update_kalman(log, freestand_meas, noise);
}

void resolver::add_shot(shot_t& shot)
{
	shots.emplace_back(shot);
}

void resolver::update_missed_shots(const ClientFrameStage_t& stage)
{
	if (stage != FRAME_NET_UPDATE_END)
		return;

	auto it = shots.begin();
	while (it != shots.end())
	{
		const auto shot = *it;
		if (shot.tick + time_to_ticks(1.f) < interfaces::globals()->tickcount || shot.tick - 10 > interfaces::globals()->tickcount)
		{
			it = shots.erase(it);
		}
		else
		{
			++it;
		}
	}

	auto it2 = current_shots.begin();
	while (it2 != current_shots.end())
	{
		const auto shot = *it2;
		if (shot.tick + time_to_ticks(1.f) < interfaces::globals()->tickcount || shot.tick - 10 > interfaces::globals()->tickcount)
		{
			it2 = current_shots.erase(it2);
		}
		else
		{
			++it2;
		}
	}
}

void resolver::hurt_listener(IGameEvent* game_event, record_shot_info_t& shot_info)
{
	const auto attacker = interfaces::engine()->GetPlayerForUserID(game_event->GetInt("attacker"));
	const auto victim = interfaces::engine()->GetPlayerForUserID(game_event->GetInt("userid"));
	const auto hitgroup = game_event->GetInt("hitgroup");
	const auto damage = game_event->GetInt("dmg_health");

	if (attacker != interfaces::engine()->GetLocalPlayer())
		return;

	if (victim == interfaces::engine()->GetLocalPlayer())
		return;

	const auto player = globals::get_player(victim);
	if (!player || !player->is_enemy())
		return;

	if (unapproved_shots.empty())
		return;

	for (auto& shot : unapproved_shots)
	{
		if (!shot.hurt && shot.enemy_index == victim)
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

resolver::shot_t* resolver::closest_shot(int tickcount)
{
	shot_t* closest_shot = nullptr;
	for (auto& shot : shots)
	{
		closest_shot = &shot;
		break;
	}

	return closest_shot;
}

bool resolver::record_shot(IGameEvent* game_event)
{
	const auto userid = interfaces::engine()->GetPlayerForUserID(game_event->GetInt("userid"));
	const auto player = globals::get_player(userid);

	if (player != local_player)
		return false;

	const auto shot = closest_shot(interfaces::globals()->tickcount - time_to_ticks(interfaces::engine()->GetNetChannelInfo()->GetLatency(FLOW_OUTGOING)));
	if (!shot)
		return false;

	current_shots.push_front(*shot);
	shots.pop_front();
	current_hitposes.clear();

	return true;
}

void resolver::listener(IGameEvent* game_event)
{
	static auto last_tickcount = 0;

	if (!strcmp(game_event->GetName(), "weapon_fire"))
	{
		if (record_shot(game_event))
			last_tickcount = 0;
		return;
	}

	if (current_shots.empty())
		return;

	const auto userid = interfaces::engine()->GetPlayerForUserID(game_event->GetInt("userid"));
	const auto player = globals::get_player(userid);

	if (!player || player != local_player)
		return;

	const Vector pos(game_event->GetFloat("x"), game_event->GetFloat("y"), game_event->GetFloat("z"));

	const auto shot = &current_shots[0];

	static auto counter = 0;

	if (last_tickcount == interfaces::globals()->tickcount)
		counter++;
	else
	{
		current_hitposes.clear();
		counter = 0;
	}

	if (counter)
		unapproved_shots.pop_front();

	current_hitposes.push_back(pos);
	shot->hitposes = current_hitposes;
	unapproved_shots.emplace_back(*shot);

	last_tickcount = interfaces::globals()->tickcount;
}

Vector resolver::get_closest_hitpos(const shot_t& shot, const Vector& pos)
{
	Vector closest = {};
	auto last_dist = FLT_MAX;
	for (auto& hitpos : shot.hitposes)
	{
		const auto dist = hitpos.Dist(pos);
		if (dist < last_dist)
		{
			last_dist = dist;
			closest = hitpos;
		}
	}

	return closest;
}

Vector resolver::get_closest_penetrationpos(const shot_t& shot, const Vector& pos)
{
	Vector closest = {};
	auto last_dist = FLT_MAX;
	for (auto& hitpos : shot.penetration_points)
	{
		const auto dist = hitpos.Dist(pos);
		if (dist < last_dist)
		{
			last_dist = dist;
			closest = hitpos;
		}
	}

	return closest;
}

void resolver::approve_shots(const ClientFrameStage_t& stage)
{
	if (stage != FRAME_NET_UPDATE_END)
		return;

	for (auto& shot : unapproved_shots)
	{
		if (shot.hitposes.empty())
			continue;

		auto end = shot.hitposes[shot.hitposes.size() - 1];

		if (vars::misc.impacts->get<bool>())
		{
			auto col2 = Color(vars::misc.impacts_color2->get<D3DCOLOR>());

			for (auto& point : shot.hitposes)
				interfaces::debug_overlay()->AddBoxOverlay(point, Vector(-1.25f, -1.25f, -1.25f), Vector(1.25f, 1.25f, 1.25f), QAngle(0, 0, 0), col2.r(), col2.g(), col2.b(), 180, 4);
		}

		if (local_player && local_player->get_alive() && prediction::get_pred_info(shot.cmdnum).sequence == shot.cmdnum)
		{
			auto new_origin = prediction::get_pred_info(shot.cmdnum).origin;
			shot.shotpos.x = new_origin.x;
			shot.shotpos.y = new_origin.y;
		}

		const auto angles = math::calc_angle(shot.shotpos, end);
		Vector direction{};
		math::angle_vectors(angles, &direction);

		if (shot.record.m_index == -1)
		{
			if (shot.hurt)
			{
				if (shot.penetration_points.empty())
					continue;

				shot.hitpos = get_closest_hitpos(shot, shot.penetration_points[shot.penetration_points.size() - 1]);
			}

			Vector zerovec = {};
			lua::api.callback(FNV1A("on_shot_registered"), [&](lua::state& state)
				{
					state.create_table();
					state.set_field(XOR_STR("manual"), true);
					state.set_field(XOR_STR("secure"), false);
					state.set_field(XOR_STR("very_secure"), false);
					state.set_field(XOR_STR("result"), shot.hurt ? XOR_STR("hit") : XOR_STR("miss"));
					state.set_field(XOR_STR("target"), -1);
					state.set_field(XOR_STR("tick"), shot.tick);
					state.set_field(XOR_STR("backtrack"), 0);
					state.set_field(XOR_STR("hitchance"), -1);
					state.set_field(XOR_STR("client_hitgroup"), -1);
					state.set_field(XOR_STR("client_damage"), -1);
					state.set_field(XOR_STR("server_hitgroup"), shot.hitinfo.hitgroup);
					state.set_field(XOR_STR("server_damage"), shot.hitinfo.damage);
					state.create_user_object<decltype(shot.shotpos)>(XOR_STR("vec3"), &shot.shotpos);
					state.set_field(XOR_STR("shotpos"));
					state.create_user_object<decltype(zerovec)>(XOR_STR("vec3"), &zerovec);
					state.set_field(XOR_STR("client_hitpos"));
					state.create_user_object<decltype(shot.hitpos)>(XOR_STR("vec3"), shot.hurt ? &shot.hitpos : &zerovec);
					state.set_field(XOR_STR("server_hitpos"));
					state.create_table();
					{
						auto index = 1;
						for (auto cur : shot.penetration_points)
						{
							state.create_user_object<decltype(cur)>(XOR_STR("vec3"), &cur);
							state.set_i(index++);
						}
					}
					state.set_field(XOR_STR("client_impacts"));
					state.create_table();
					{
						auto index = 1;
						for (auto cur : shot.hitposes)
						{
							state.create_user_object<decltype(cur)>(XOR_STR("vec3"), &cur);
							state.set_i(index++);
						}
					}
					state.set_field(XOR_STR("server_impacts"));
					return 1;
				});

			if (shot.hurt)
			{
				const auto player = globals::get_player(shot.hitinfo.victim);
				if (player)
				{
					add_hit(hitmarker::hitmarker_t(interfaces::globals()->realtime, shot.hitinfo.victim, shot.hitinfo.damage, shot.hitinfo.hitgroup, shot.hitpos));

					if (vars::visuals.beams.local.enabled->get<bool>() && vars::visuals.beams.enabled->get<bool>())
						add_local_beam(beams::impact_info_t(interfaces::globals()->curtime, shot.shotpos, end, interfaces::engine()->GetLocalPlayer(), Color(vars::visuals.beams.local.color->get<D3DCOLOR>())));
					continue;
				}
			}

			if (vars::visuals.beams.local.enabled->get<bool>() && vars::visuals.beams.enabled->get<bool>())
				add_local_beam(beams::impact_info_t(interfaces::globals()->curtime, shot.shotpos, end, interfaces::engine()->GetLocalPlayer(), Color(vars::visuals.beams.local.color->get<D3DCOLOR>())));
			continue;
		}

		auto hitpos = get_closest_hitpos(shot, shot.hitgroup != -1 ? shot.hitpos : shot.record.m_origin);

		auto player = globals::get_player(shot.enemy_index);
		if (vars::visuals.chams.enemy.shot_record.type->get<int>() && player)
			chams::add_ghost(player, &shot.record);

		if (!player)
		{
			// maybe add shot info

			shot.hitpos = hitpos;
			if (shot.hurt)
			{
				add_hit(hitmarker::hitmarker_t(interfaces::globals()->realtime, shot.hitinfo.victim, shot.hitinfo.damage, shot.hitinfo.hitgroup, hitpos));

				if (vars::visuals.beams.local.enabled->get<bool>() && vars::visuals.beams.enabled->get<bool>() && !beams::beam_exists(local_player, interfaces::globals()->curtime))
					add_local_beam(beams::impact_info_t(interfaces::globals()->curtime, shot.shotpos, end, interfaces::engine()->GetLocalPlayer(), Color(vars::visuals.beams.local.color->get<D3DCOLOR>())));
			}
			else if (vars::visuals.beams.local.enabled->get<bool>() && vars::visuals.beams.enabled->get<bool>())
				add_local_beam(beams::impact_info_t(interfaces::globals()->curtime, shot.shotpos, end, interfaces::engine()->GetLocalPlayer(), Color(vars::visuals.beams.local.color->get<D3DCOLOR>())));

			continue;
		}

		if (!local_player || !local_player->get_alive() || !local_weapon)
			continue;

		shot.hitpos = shot.hitposes[shot.hitposes.size() - 1] + direction * 1000.f;

		auto& log = player_log::get_log(shot.enemy_index);
		auto data = penetration::pen_data(&shot.record, shot.record.m_shot_dir, false, nullptr, &shot.weapon_data);

		if (shot.record.m_shot_info.extrapolated && !log.record.empty() && !log.record.back().m_dormant)
		{
			//aimbot_helpers::draw_debug_hitboxes( player, log.record.back().matrix( shot.record.m_shot_state ), -1, 5.f, Color( 0, 255, 255, 255 ) );
			data = penetration::pen_data(&log.record.back(), shot.record.m_shot_dir, false, nullptr, &shot.weapon_data);
		}

		aimbot::aimpoint_t aimpoint;
		aimpoint.hitbox = -1;
		aimpoint.point = end;

		auto damage = 0;
		auto new_data = data;
		if (can_hit(local_player, new_data, shot.shotpos, &aimpoint, damage, true))
		{
			hitpos = get_closest_hitpos(shot, aimpoint.point);
			shot.hitpos = hitpos;
			shot.hit = true;
			shot.hit_originally = true;
		}

		const auto deltavec = Vector(shot.original_shotpos.x - shot.shotpos.x, shot.original_shotpos.y - shot.shotpos.y, 0);
		const auto corrected_pos = fabsf(deltavec.x) >= 0.001f || fabsf(deltavec.y) >= 0.001f;

		if (corrected_pos)
		{
			auto damage2 = 0;
			shot.hit_originally = can_hit(local_player, data, shot.original_shotpos, &aimpoint, damage2, true);
		}

		if (shot.record.m_shot_info.extrapolated)
		{
			//aimbot_helpers::draw_debug_hitboxes( player, shot.record.matrix( shot.record.m_shot_state ), -1, 5.f, Color( 255, 255, 255, 255 ) );

			auto damage2 = 0;
			shot.hit_extrapolation = can_hit(local_player, penetration::pen_data(&shot.record, shot.record.m_shot_dir, false, nullptr, &shot.weapon_data), shot.shotpos, &aimpoint, damage2, true);
		}

		if (vars::visuals.beams.local.enabled->get<bool>() && vars::visuals.beams.enabled->get<bool>())
			add_local_beam(beams::impact_info_t(interfaces::globals()->curtime, shot.shotpos, end, interfaces::engine()->GetLocalPlayer(), Color(vars::visuals.beams.local.color->get<D3DCOLOR>())));

		if (shot.hurt)
			add_hit(hitmarker::hitmarker_t(interfaces::globals()->realtime, shot.hitinfo.victim, shot.hitinfo.damage, shot.hitinfo.hitgroup, hitpos));

		if (shot.hitgroup == -1)
			continue;

		Vector zerovec = {};

		lua::api.callback(FNV1A("on_shot_registered"), [&](lua::state& state)
			{
				state.create_table();
				state.set_field(XOR_STR("manual"), shot.hitgroup == -1);
				state.set_field(XOR_STR("secure"), shot.safety >= penetration::safety_no_roll);
				state.set_field(XOR_STR("very_secure"), shot.safety >= penetration::safety_full);
				state.set_field(XOR_STR("result"), shot.hurt ? XOR_STR("hit") : shot.hit ? XOR_STR("resolve") : shot.hit_extrapolation ? (!ConVar::cl_lagcompensation || !ConVar::cl_predict) ? XOR_STR("anti-exploit") : XOR_STR("extrapolation") : shot.hit_originally ? XOR_STR("server correction") : XOR_STR("spread"));
				state.set_field(XOR_STR("target"), shot.enemy_index);
				state.set_field(XOR_STR("tick"), shot.tick);
				state.set_field(XOR_STR("backtrack"), shot.record.m_shot_info.backtrack_ticks);
				state.set_field(XOR_STR("hitchance"), shot.record.m_shot_info.hitchance);
				state.set_field(XOR_STR("client_hitgroup"), shot.hitgroup);
				state.set_field(XOR_STR("client_damage"), shot.damage);
				state.set_field(XOR_STR("server_hitgroup"), shot.hitinfo.hitgroup);
				state.set_field(XOR_STR("server_damage"), shot.hitinfo.damage);
				state.create_user_object<decltype(shot.shotpos)>(XOR_STR("vec3"), &shot.shotpos);
				state.set_field(XOR_STR("shotpos"));
				state.create_user_object<decltype(end)>(XOR_STR("vec3"), &end);
				state.set_field(XOR_STR("client_hitpos"));
				state.create_user_object<decltype(shot.hitpos)>(XOR_STR("vec3"), shot.hurt ? &shot.hitpos : &zerovec);
				state.set_field(XOR_STR("server_hitpos"));
				state.create_table();
				{
					auto index = 1;
					for (auto cur : shot.penetration_points)
					{
						state.create_user_object<decltype(cur)>(XOR_STR("vec3"), &cur);
						state.set_i(index++);
					}
				}
				state.set_field(XOR_STR("client_impacts"));
				state.create_table();
				{
					auto index = 1;
					for (auto cur : shot.hitposes)
					{
						state.create_user_object<decltype(cur)>(XOR_STR("vec3"), &cur);
						state.set_i(index++);
					}
				}
				state.set_field(XOR_STR("server_impacts"));
				return 1;
			});

		if (player->get_player_info().fakeplayer)
		{
			calc_missed_shots(&shot);

			continue;
		}

		if (vars::legit_enabled())
			continue;

		get_brute_angle(&shot);

		calc_missed_shots(&shot);
	}

	current_shots.clear();
	unapproved_shots.clear();
	current_hitposes.clear();
}



void resolver::get_brute_angle(shot_t* shot)
{
	if (!local_player || !local_player->get_alive() || !local_weapon || shot->record.m_dormant)
		return;

	const auto player = globals::get_player(shot->enemy_index);
	if (!player || !player->get_alive())
		return;

	if (player->get_player_info().fakeplayer)
		return;

	if (vars::legit_enabled())
		return;

	auto& log = player_log::get_log(shot->enemy_index);

	const auto mode = shot->record.m_shot ? resolver_mode::resolver_shot : shot->record.m_resolver_mode;
	const auto side = shot->record.m_resolver_side;
	const auto tried_dir = shot->record.m_shot_dir;

	// Biases matched to bias_to_direction thresholds:
	//   min        < -0.55  (band center ~ -0.66)
	//   min_extra  < -0.32  (band center ~ -0.43)
	//   max_extra  >  0.32  (band center ~  0.43)
	//   max        >  0.55  (band center ~  0.66)
	// min_min / max_max removed
	float tried_bias = 0.f;
	switch (tried_dir)
	{
	case resolver_direction::resolver_min:       tried_bias = -0.66f; break;
	case resolver_direction::resolver_min_extra: tried_bias = -0.43f; break;
	case resolver_direction::resolver_max:       tried_bias = 0.66f; break;
	case resolver_direction::resolver_max_extra: tried_bias = 0.43f; break;
	default:                                     tried_bias = 0.f;   break;
	}

	const bool aimed_head_hit_body =
		shot->hurt &&
		shot->hitgroup == HITGROUP_HEAD &&
		shot->hitinfo.hitgroup != HITGROUP_HEAD;

	const bool was_extrapolated =
		shot->record.m_extrapolated || shot->record.m_shot_info.extrapolated;

	const bool registered_hit = shot->hurt && !aimed_head_hit_body;
	const bool registered_miss = (shot->hit && !shot->hurt) || aimed_head_hit_body;
	const bool no_registration = !shot->hit && !shot->hurt;

	// ---------- Kalman update ----------
	if (registered_hit)
	{
		float hurt_noise;
		switch (shot->hitgroup)
		{
		case HITGROUP_HEAD:                                      hurt_noise = 0.04f; break;
		case HITGROUP_CHEST:                                     hurt_noise = 0.06f; break;
		case HITGROUP_STOMACH:                                   hurt_noise = 0.07f; break;
		case HITGROUP_LEFTARM: case HITGROUP_RIGHTARM:
		case HITGROUP_LEFTLEG: case HITGROUP_RIGHTLEG:           hurt_noise = 0.09f; break;
		default:                                                 hurt_noise = 0.07f; break;
		}

		if (was_extrapolated)
			hurt_noise = std::min(hurt_noise + 0.05f, 0.15f);

		update_kalman(log, tried_bias, hurt_noise);
	}
	else if (registered_miss)
	{
		if (fabsf(tried_bias) >= 0.05f)
		{
			const float commitment = 1.f - std::clamp(log.m_kalman.variance / 0.25f, 0.f, 1.f);
			float miss_noise = aimed_head_hit_body
				? 0.10f - commitment * 0.04f
				: 0.08f - commitment * 0.02f;

			if (was_extrapolated)
				miss_noise = std::min(miss_noise + 0.08f, 0.40f);

			update_kalman(log, -tried_bias, miss_noise);
		}
	}
	else // no_registration
	{
		if (fabsf(tried_bias) >= 0.05f)
		{
			const float commitment = 1.f - std::clamp(log.m_kalman.variance / 0.25f, 0.f, 1.f);
			float noreg_noise = 0.25f - commitment * 0.10f;

			if (was_extrapolated)
				noreg_noise = std::min(noreg_noise + 0.10f, 0.45f);

			update_kalman(log, -tried_bias, noreg_noise);
		}
	}

	// ---------- Blacklist ----------
	if (!no_registration)
	{
		auto& bl = log.m_mode[mode].m_side[side].m_blacklist;
		const float bias = log.m_kalman.bias;

		if (registered_miss)
			bl[tried_dir] = true;

		if (registered_hit)
			bl[tried_dir] = false;

		const float hysteresis = 0.35f;

		// Only the directions bias_to_direction still uses
		if (bl[resolver_direction::resolver_min_extra] && bias > -0.55f + hysteresis)
			bl[resolver_direction::resolver_min_extra] = false;
		if (bl[resolver_direction::resolver_min] && bias > -0.32f + hysteresis)
			bl[resolver_direction::resolver_min] = false;

		if (bl[resolver_direction::resolver_max_extra] && bias < 0.55f - hysteresis)
			bl[resolver_direction::resolver_max_extra] = false;
		if (bl[resolver_direction::resolver_max] && bias < 0.32f - hysteresis)
			bl[resolver_direction::resolver_max] = false;
	}

	// ---------- Unknown flag handling ----------
	if (log.m_mode[mode].m_side[side].m_current_dir != tried_dir || registered_hit)
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

	// ---------- Direction name ----------
	const auto dir = shot->record.m_shot_dir;
	const char* dir_name = "unknown";
	switch (dir)
	{
	case resolver_direction::resolver_networked: dir_name = "networked"; break;
	case resolver_direction::resolver_max:        dir_name = "max"; break;
	case resolver_direction::resolver_zero:       dir_name = "zero"; break;
	case resolver_direction::resolver_min:        dir_name = "min"; break;
	case resolver_direction::resolver_max_extra:  dir_name = "max_extra"; break;
	case resolver_direction::resolver_max_max:    dir_name = "max_max"; break;
	case resolver_direction::resolver_min_min:    dir_name = "min_min"; break;
	case resolver_direction::resolver_min_extra:  dir_name = "min_extra"; break;
	}

	// ---------- Main log line ----------
	interfaces::cvar()->ConsoleColorPrintf(Color(235, 5, 90), xorstr_("[fatality] "));

	if (shot->hurt)
	{
		const bool mismatch = shot->hitgroup == HITGROUP_HEAD &&
			shot->hitinfo.hitgroup != HITGROUP_HEAD;

		if (mismatch)
		{
			util::print_dev_console(true, Color(255, 150, 50),
				xorstr_("mismatch due to resolver - dir=%-10s bias=%+.2f var=%.3f safety=%d dmg=%d\n"),
				dir_name, log.m_kalman.bias, log.m_kalman.variance, shot->safety, shot->damage);
		}
		else
		{
			util::print_dev_console(true, Color(100, 255, 100),
				xorstr_("hit resolved shot - dir=%-10s bias=%+.2f var=%.3f safety=%d dmg=%d\n"),
				dir_name, log.m_kalman.bias, log.m_kalman.variance, shot->safety, shot->damage);
		}
	}
	else if (shot->hit)
	{
		util::print_dev_console(true, Color(255, 80, 80),
			xorstr_("missed shot due to resolver - dir=%-10s bias=%+.2f var=%.3f safety=%d dmg=%d\n"),
			dir_name, log.m_kalman.bias, log.m_kalman.variance, shot->safety, shot->damage);
	}
	else if (shot->hit_extrapolation)
	{
		const char* reason = (!ConVar::cl_lagcompensation || !ConVar::cl_predict) ? "anti-exploit" : "extrapolation";
		util::print_dev_console(true, Color(255, 140, 50),
			xorstr_("missed shot due to %s - dir=%-10s bias=%+.2f var=%.3f safety=%d dmg=%d\n"),
			reason, dir_name, log.m_kalman.bias, log.m_kalman.variance, shot->safety, shot->damage);
	}
	else if (shot->hit_originally)
	{
		util::print_dev_console(true, Color(255, 200, 50),
			xorstr_("missed shot due to server correction - dir=%-10s bias=%+.2f var=%.3f safety=%d dmg=%d\n"),
			dir_name, log.m_kalman.bias, log.m_kalman.variance, shot->safety, shot->damage);
	}
	else
	{
		util::print_dev_console(true, Color(255, 180, 80),
			xorstr_("missed shot due to spread - dir=%-10s bias=%+.2f var=%.3f safety=%d dmg=%d\n"),
			dir_name, log.m_kalman.bias, log.m_kalman.variance, shot->safety, shot->damage);
	}

	// ---------- Dormant handling ----------
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
				xorstr_("missed shot due to dormant aimbot (total: %d)\n"), log.m_dormant_misses);
		}
		return;
	}

	// ---------- Nospread pitch cycle ----------
	if (shot->hurt && globals::nospread && shot->hitinfo.hitgroup == HITGROUP_HEAD && !shot->record.m_shot)
		log.nospread.m_pitch_cycle = 0;

	// ---------- Early outs / bookkeeping ----------
	if (shot->hurt)
		return;

	const auto player = globals::get_player(shot->enemy_index);

	if (shot->hit && player && player->get_alive())
	{
		if (shot->record.m_unknown)
			log.m_unknown_misses++;

		log.m_shots++;
		return;
	}

	log.m_shots_spread++;
}

void resolver::set_local_info()
{
	last_origin_diff = local_player->get_origin() - last_origin;
	last_eye = local_player->get_eye_pos();
	last_origin = local_player->get_origin();
	current_eye = local_player->get_eye_pos();
}
