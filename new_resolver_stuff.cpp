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
	float process_noise = 0.028f; // base (slightly sticky)

	// More responsive when desync is clearly active
	if (anim.layer6_weight > 0.60f || fabsf(anim.eye_feet_delta) > 45.f)
		process_noise = 0.055f;

	// Stickier when standing still with stable desync
	if (anim.is_standing && anim.standing_ticks > 8 && fabsf(anim.eye_feet_delta) < 25.f)
		process_noise = 0.018f;

	// Prediction step
	k.variance += process_noise;

	// Update step
	const float innovation = measurement - k.bias;
	const float innovation_var = k.variance + measurement_noise;

	// Cap gain so a single measurement can't completely override the filter
	const float gain = std::min(k.variance / innovation_var, 0.85f);

	k.bias += gain * innovation;
	k.variance *= (1.f - gain);

	// Very gentle decay on weak measurements so we don't stay locked forever
	if (measurement_noise > 0.60f)
		k.bias *= 0.993f;

	k.bias = std::clamp(k.bias, -1.f, 1.f);
}

void resolver::update_kalman_from_anim(player_log_t& log)
{
	const auto& anim = log.m_anim;

	float measurement = 0.f;
	float noise = 0.90f; // default = very uncertain

	if (anim.is_standing && anim.standing_ticks > 2)
	{
		// Best quality signal
		measurement = std::clamp(anim.eye_feet_delta / 58.f, -1.f, 1.f);
		noise = 0.25f;

		// Even higher trust when layer 6 is heavy
		if (anim.layer6_weight > 0.55f)
			noise = 0.18f;
	}
	else if (anim.is_moving)
	{
		measurement = std::clamp(anim.velocity_yaw_delta / 65.f, -1.f, 1.f);
		noise = 0.65f;
	}
	else
	{
		// Air / unknown
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
	const bool strong_feet_flip = feet_delta_change > 40.f;
	const bool extreme_desync = fabsf(anim.eye_feet_delta) > 120.f;

	const auto previous_mode = log.m_current_mode;

	if (strong_layer_flip || strong_feet_flip || extreme_desync)
		log.m_current_mode = static_cast<resolver_mode>(!static_cast<int>(log.m_current_mode));

	if (previous_mode != log.m_current_mode)
		log.m_last_flip_tick = interfaces::client_state()->get_last_server_tick();

	if (interfaces::client_state()->get_last_server_tick() - log.m_last_flip_tick > time_to_ticks(1.1f))
		log.m_current_mode = resolver_mode::resolver_default;
}
