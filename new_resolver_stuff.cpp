#include <random>
#include "../include_cheat.h"

void resolver::resolve( C_CSPlayer* player, lag_record_t* record, lag_record_t* previous )
{
	if ( !player->is_enemy() )
		return;

	if ( !record->m_shot )
		pitch_resolve( record );

	auto& log = player_log::get_log(player->EntIndex());   // ← add this
	update_animation_features(log, record);
	update_kalman_from_anim(log);

	yaw_resolve( record, previous );
}

void resolver::post_animate(C_CSPlayer* player, lag_record_t* record)
{
	auto& log = player_log::get_log(player->EntIndex());

	// Keep existing safety against invalid directions
	if (vars::aim.resolver_mode->get<int>())
	{
		for (auto& mode : log.m_mode)
		{
			for (auto& side : mode.m_side)
			{
				if (side.m_current_dir > resolver_direction::resolver_max)
				{
					side.m_current_dir = resolver_direction::resolver_networked;
					log.m_unknown_shot = true;
					log.m_unknown = true;
				}
			}
		}
	}

	// Reset sides for non-enemies / bots
	if (!player->is_enemy() || player->get_player_info().fakeplayer)
	{
		log.m_mode[resolver_mode::resolver_shot].m_side =
			log.m_mode[resolver_mode::resolver_default].m_side =
			log.m_mode[resolver_mode::resolver_flip].m_side = {};
	}

	// Store mode/side on the record
	record->m_resolver_mode = record->m_shot ? resolver_mode::resolver_shot : log.m_current_mode;
	record->m_resolver_side = log.m_current_side;

	// Pitch tracking
	if (!record->m_shot)
	{
		const auto cureye = record->m_eye_angles;
		if (fabsf(cureye.x) >= 60.f)
			log.m_last_unusual_pitch = interfaces::globals()->curtime;
		else
			log.m_last_zero_pitch = interfaces::globals()->curtime;
	}

	// Copy direction into shot mode if needed (existing logic)
	if (log.m_unknown_shot && log.m_mode[log.m_current_mode].m_side[log.m_current_side].m_current_dir > resolver_direction::resolver_networked)
	{
		log.m_mode[resolver_mode::resolver_shot].m_side[log.m_current_side].m_current_dir =
			log.m_mode[log.m_current_mode].m_side[log.m_current_side].m_current_dir;
	}

	// ---------- Apply Kalman direction (FIXED) ----------
	// Write into the same mode the record will actually use
	if (!record->m_shot && player->is_enemy() && !player->get_player_info().fakeplayer)
	{
		const auto mode_to_write = record->m_resolver_mode; // already set above
		const auto dir = bias_to_direction(log.m_kalman.bias);

		log.m_mode[mode_to_write].m_side[log.m_current_side].m_current_dir = dir;
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

	// ========================================================================
	// SIMPLE PATH (higher quality movement prediction)
	// ========================================================================
	if (simple)
	{
		process_move_changes_t backup_pm{};
		backup_pm.store(player);

		const auto& original_record = log.record.back();
		const lag_record_t* p1 = log.record.size() > 1 ? &log.record[log.record.size() - 2] : nullptr;
		const lag_record_t* p2 = log.record.size() > 2 ? &log.record[log.record.size() - 3] : nullptr;

		// Improved velocity trend
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

		// Clamp insane acceleration
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

			// Ducking
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

			// Air strafe correction
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
			// Ground speed control
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

			// Simulate one tick
			CMoveData data = interfaces::game_movement()->setup_move(player, &cmd);
			data.m_nOldButtons = prev_buttons;
			const auto ret = interfaces::game_movement()->process_movement(player, &data);
			prev_buttons = data.m_nButtons;
			ret.restore(player);

			// Jump handling
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

	// ========================================================================
	// NON-SIMPLE PATH (animation update)
	// ========================================================================
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
	anim.feet_yaw = animstate.foot_yaw;				// matches what you already use

	anim.eye_feet_delta = math::normalize_float(anim.eye_yaw - anim.feet_yaw);

	// ----- Movement state -----
	const Vector& vel = record->m_calculated_velocity.Length2D() > 1.f
		? record->m_calculated_velocity
		: record->m_velocity;

	const float speed_2d = vel.Length2D();

	anim.is_moving = speed_2d > 5.f;
	anim.is_standing = !anim.is_moving && (record->m_flags & FL_ONGROUND);

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

	// ----- Extra from addon -----
	anim.adjust_weight = record->addon.adjust_weight;
}

void resolver::update_kalman(player_log_t& log, float measurement, float measurement_noise)
{
	auto& k = log.m_kalman;
	const auto& anim = log.m_anim;

	// ---------- Adaptive process noise ----------
	float process_noise = 0.028f;

	if (anim.layer6_weight > 0.60f || fabsf(anim.eye_feet_delta) > 45.f)
		process_noise = 0.055f;

	if (anim.is_standing && anim.standing_ticks > 8 && fabsf(anim.eye_feet_delta) < 25.f)
		process_noise = 0.018f;

	k.variance += process_noise;

	// [CHANGE] Variance floor — prevents the filter getting overconfident
	// after a long streak of low-noise measurements and then overcorrecting
	k.variance = std::max(k.variance, 0.004f);

	const float innovation = measurement - k.bias;
	const float innovation_var = k.variance + measurement_noise;

	// [CHANGE] Lowered gain cap 0.85 → 0.78 — a single measurement
	// (especially a shot feedback one) was slightly too dominant
	const float gain = std::min(k.variance / innovation_var, 0.78f);

	k.bias += gain * innovation;
	k.variance *= (1.f - gain);

	if (measurement_noise > 0.60f)
		k.bias *= 0.993f;

	k.bias = std::clamp(k.bias, -1.f, 1.f);
}


void resolver::update_kalman_from_anim(player_log_t& log)
{
	const auto& anim = log.m_anim;

	float measurement = 0.f;
	float noise = 0.90f;

	if (anim.is_standing && anim.standing_ticks > 2)
	{
		measurement = std::clamp(anim.eye_feet_delta / 58.f, -1.f, 1.f);
		noise = 0.25f;

		if (anim.layer6_weight > 0.55f)
			noise = 0.18f;

		// [CHANGE] When layer 12 also has significant weight it means the
		// adjustment layer is active and eye_feet_delta is especially reliable.
		// Tighten noise further rather than staying at 0.18.
		if (anim.layer6_weight > 0.55f && anim.layer12_weight > 0.40f)
			noise = 0.13f;

		// [CHANGE] Very stable standing — allow the filter to lock in harder
		if (anim.standing_ticks > 20 && fabsf(anim.eye_feet_delta) > 15.f)
			noise = std::max(noise - 0.04f, 0.09f);
	}
	else if (anim.is_moving)
	{
		measurement = std::clamp(anim.velocity_yaw_delta / 65.f, -1.f, 1.f);
		noise = 0.65f;
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
	// Strong left
	if (bias < -0.78f)
		return resolver_direction::resolver_min_min;
	if (bias < -0.55f)
		return resolver_direction::resolver_min;
	if (bias < -0.32f)
		return resolver_direction::resolver_min_extra;

	// Strong right
	if (bias > 0.78f)
		return resolver_direction::resolver_max_max;
	if (bias > 0.55f)
		return resolver_direction::resolver_max;
	if (bias > 0.32f)
		return resolver_direction::resolver_max_extra;

	// Near zero → prefer the neutral states
	if (fabsf(bias) < 0.12f)
		return resolver_direction::resolver_zero;

	// Mild bias still goes to networked (safe default)
	return resolver_direction::resolver_networked;
}

void resolver::yaw_resolve(const lag_record_t* record, const lag_record_t* previous)
{
	if (record->m_shot || (previous && previous->m_shot))
		return;

	auto& log = player_log::get_log(record->m_index);
	const auto& anim = log.m_anim;

	float layer6_delta = 0.f;
	float feet_delta_change = 0.f;

	if (previous)
	{
		layer6_delta = fabsf(record->m_layers[6].m_flWeight - previous->m_layers[6].m_flWeight);

		const float prev_feet = previous->m_state[resolver_direction::resolver_networked].m_animstate.foot_yaw;
		const float prev_eye_feet = math::normalize_float(previous->m_eye_angles.y - prev_feet);
		feet_delta_change = fabsf(anim.eye_feet_delta - prev_eye_feet);
	}

	const bool standing = anim.is_standing && anim.standing_ticks > 1;
	const bool strong_layer_flip = standing && layer6_delta > 0.45f;

	// [CHANGE] Gate feet flip on !is_moving — a large feet delta on a moving
	// player is often just normal locomotion yaw catching up, not a real flip.
	// Previously this could fire spuriously on direction changes while running.
	const bool strong_feet_flip = !anim.is_moving && feet_delta_change > 40.f;

	// [CHANGE] Lowered extreme desync threshold 120 → 105.
	// 120° is unreachably extreme in practice; 105° still covers
	// max-desync while catching edge cases a tick earlier.
	const bool extreme_desync = fabsf(anim.eye_feet_delta) > 105.f;

	const auto previous_mode = log.m_current_mode;

	if (strong_layer_flip || strong_feet_flip || extreme_desync)
		log.m_current_mode = static_cast<resolver_mode>(!static_cast<int>(log.m_current_mode));

	if (previous_mode != log.m_current_mode)
		log.m_last_flip_tick = interfaces::client_state()->get_last_server_tick();

	if (interfaces::client_state()->get_last_server_tick() - log.m_last_flip_tick > time_to_ticks(1.1f))
		log.m_current_mode = resolver_mode::resolver_default;
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

	// ---------- Lightweight freestand ----------
	const Vector eye_pos = record->m_origin + Vector(0.f, 0.f, 64.f);	// rough head height
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

	// Simple damage check (you can keep using can_hit / pen_data if you prefer)
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
	// Positive bias = right, negative = left
	float freestand_meas = 0.f;
	float noise = 0.55f;		// medium trust

	if (dmg_left > 0.f || dmg_right > 0.f)
	{
		const float total = dmg_left + dmg_right + 1.f;
		freestand_meas = (dmg_right - dmg_left) / total;	// roughly -1 ... +1
		noise = 0.35f;		// we have real freestand info → higher trust
	}

	update_kalman(log, freestand_meas, noise);
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

	// [CHANGE] Differentiated min_min from min_extra and max_max from max_extra.
	// Previously both bucketed to ±0.95, losing the distinction between
	// "very wrong" and "slightly wrong" when pushing the filter from misses.
	float tried_bias = 0.f;
	switch (tried_dir)
	{
	case resolver_direction::resolver_min_min:
		tried_bias = -0.95f; break;
	case resolver_direction::resolver_min_extra:
		tried_bias = -0.80f; break;
	case resolver_direction::resolver_min:
		tried_bias = -0.70f; break;
	case resolver_direction::resolver_max_max:
		tried_bias = 0.95f; break;
	case resolver_direction::resolver_max_extra:
		tried_bias = 0.80f; break;
	case resolver_direction::resolver_max:
		tried_bias = 0.70f; break;
	default:
		tried_bias = 0.f;   break;
	}

	// ---------- Kalman update ----------
	if (shot->hurt)
	{
		// [CHANGE] Weight noise by hitgroup. A headshot is a tight geometric
		// confirmation of the correct yaw; a foot graze on the hitbox edge is not.
		float hurt_noise;
		switch (shot->hitgroup)
		{
		case HITGROUP_HEAD:
			hurt_noise = 0.04f; break;
		case HITGROUP_CHEST:
			hurt_noise = 0.06f; break;
		case HITGROUP_LEFTARM:
		case HITGROUP_RIGHTARM:
		case HITGROUP_LEFTLEG:
		case HITGROUP_RIGHTLEG:
			hurt_noise = 0.09f; break;
		default:
			hurt_noise = 0.07f; break;
		}

		update_kalman(log, tried_bias, hurt_noise);
	}
	else if (shot->hit)
	{
		// Resolver miss — push away from what we tried.
		update_kalman(log, -tried_bias, 0.09f);
	}

	// ---------- Blacklist ----------
	if (!shot->hurt && shot->hit)
		log.m_mode[mode].m_side[side].m_blacklist[tried_dir] = true;

	if (shot->hurt)
		log.m_mode[mode].m_side[side].m_blacklist[tried_dir] = false;

	// [CHANGE] Passive blacklist expiry — if the Kalman bias has since drifted
	// to the opposite side of a blacklisted direction, the entry is stale and
	// holding it open would block recovery after a real side switch.
	// This requires no new struct fields: we just check if the filter already
	// disagrees with what the entry is blocking.
	auto& bl = log.m_mode[mode].m_side[side].m_blacklist;
	const float bias = log.m_kalman.bias;

	if (bl[resolver_direction::resolver_min_min] && bias > 0.20f) bl[resolver_direction::resolver_min_min] = false;
	if (bl[resolver_direction::resolver_min_extra] && bias > 0.20f) bl[resolver_direction::resolver_min_extra] = false;
	if (bl[resolver_direction::resolver_min] && bias > 0.20f) bl[resolver_direction::resolver_min] = false;
	if (bl[resolver_direction::resolver_max_max] && bias < -0.20f) bl[resolver_direction::resolver_max_max] = false;
	if (bl[resolver_direction::resolver_max_extra] && bias < -0.20f) bl[resolver_direction::resolver_max_extra] = false;
	if (bl[resolver_direction::resolver_max] && bias < -0.20f) bl[resolver_direction::resolver_max] = false;

	// ---------- Unknown flag handling ----------
	if (log.m_mode[mode].m_side[side].m_current_dir != tried_dir || shot->hurt)
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
		util::print_dev_console(true, Color(100, 255, 100),
			xorstr_("resolved shot - dir=%-10s bias=%+.2f var=%.3f safety=%d dmg=%d\n"),
			dir_name, log.m_kalman.bias, log.m_kalman.variance, shot->safety, shot->damage);
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
