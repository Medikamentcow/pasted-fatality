#include "../include_cheat.h"

void tickbase::reset()
{
	to_recharge = to_shift = to_adjust = 0;
	delay_shift = -1;
	force_choke = force_unchoke = skip_next_adjust = fast_fire = hide_shot = post_shift = keep_config_changed = false;
}

bool tickbase::holds_tick_base_weapon()
{
	const auto wpn = local_weapon;
	if (!wpn)
		return false;

	const auto info = interfaces::weapon_system()->GetWpnData(wpn->get_weapon_id());

	if (!info)
		return false;

	return wpn->get_weapon_id() != WEAPON_TASER
		&& wpn->get_weapon_id() != WEAPON_FISTS
		&& wpn->get_weapon_id() != WEAPON_C4
		&& !wpn->is_grenade()
		&& wpn->GetClientClass()->m_ClassID != ClassId::CSnowball
		&& wpn->get_weapon_type() != WEAPONTYPE_UNKNOWN;
}

void tickbase::adjust_limit_dynamic(CUserCmd* cmd)
{
	const auto changed = apply_static_configuration();
	const auto ready = !to_shift && !post_shift && !force_choke;

	if (changed)
		keep_config_changed = force_unchoke = true;

	const auto wpn = local_weapon;
	if (!wpn || !ready || animations::most_recent.second != interfaces::client_state()->lastoutgoingcommand)
		return;

	// === Features are currently OFF → instant full discharge ===
	if (!fast_fire && !hide_shot)
	{
		to_recharge = 0;

		const int current = compute_current_limit();
		if (current > 0)
			to_shift = current;          // dump everything immediately (teleport)
		else
			to_shift = 0;

		keep_config_changed = false;
		return;
	}

	// === Features are ON ===

	const auto info = interfaces::weapon_system()->GetWpnData(wpn->get_weapon_id());
	if (!info)
		return;

	bool dont_recharge = false;

	if (wpn->is_grenade() && (wpn->get_pin_pulled() || wpn->get_throw_time() != 0.f))
		dont_recharge = true;

	if (aimbot::last_target != -1 || prediction::had_attack || cmd->weaponselect)
		dont_recharge = true;

	const float diff_wpn = wpn->get_next_primary_attack() - interfaces::globals()->curtime;
	const float diff_player = local_player->get_next_attack() - interfaces::globals()->curtime;

	if (!dont_recharge && !changed && (wpn->is_shootable() || wpn->is_knife()))
	{
		if ((info->cycle_time < 0.55f && diff_wpn > -0.2f) || diff_wpn > 0.7f)
		{
			if (wpn->is_knife() || !wpn->in_reload())
				dont_recharge = true;
		}
	}

	if (!dont_recharge && diff_player > 0.7f)
		dont_recharge = true;

	if (keep_config_changed)
		dont_recharge = false;

	if (dont_recharge)
	{
		to_recharge = 0;
		return;
	}

	const int optimal = determine_optimal_limit();
	const int current = compute_current_limit();
	const int diff = optimal - current;

	constexpr int deadzone = 2;

	if (diff > deadzone)
	{
		to_recharge = diff;
		to_shift = 0;
	}
	else if (diff < -deadzone)
	{
		to_recharge = 0;
		to_shift = -diff;
	}
	else
	{
		// Stabilize
		if (to_recharge > 0 && current >= optimal - 1)
			to_recharge = 0;

		if (to_shift > 0 && current <= optimal + 1)
			to_shift = 0;
	}

	if (diff == 0)
		keep_config_changed = false;
}

bool tickbase::attempt_shift_back(bool& send_packet)
{
	const auto weapon = local_weapon;
	if (!weapon)
		return true;

	const auto is_revolver = weapon->get_weapon_id() == WEAPON_REVOLVER;
	const int cur_limit = compute_current_limit();

	// Weapon-aware threshold (point 3)
	// Fast weapons need to shift back earlier so they finish recharging before next fire window
	int shift_back_threshold = 3;

	if (weapon->is_knife())
		shift_back_threshold = 4;
	else if (weapon->get_weapon_id() == WEAPON_REVOLVER)
		shift_back_threshold = 5;
	else
	{
		const auto info = interfaces::weapon_system()->GetWpnData(weapon->get_weapon_id());
		if (info)
		{
			const float cycle = info->cycle_time;
			if (cycle < 0.25f)          // very fast weapons (pistols, etc.)
				shift_back_threshold = 2;
			else if (cycle < 0.45f)
				shift_back_threshold = 3;
			else
				shift_back_threshold = 4;
		}
	}

	const bool dont = (fast_fire || hide_shot) && is_revolver
		|| globals::shot_command <= interfaces::client_state()->lastoutgoingcommand
		|| to_shift > 0;

	if (cur_limit > shift_back_threshold && local_player->get_tickbase() > animations::lag.first && !dont)
	{
		const auto predicted_time = interfaces::globals()->curtime + ticks_to_time(cur_limit);
		const auto release_tick = time_to_ticks(weapon->get_next_secondary_attack() - predicted_time);

		skip_next_adjust = !is_revolver || (release_tick > 1 && release_tick < 10 - interfaces::client_state()->chokedcommands);

		if (skip_next_adjust)
			send_packet = true;

		if (!resolver::shots.empty())
			resolver::shots.pop_back();

		prediction::take_shot(false);
		if (!is_revolver)
			prediction::take_secondary_shot(false);

		globals::shot_command = 0;
		misc::retract_peek = false;

		return false;
	}

	// Fast-fire / doubletap path
	if (fast_fire)
	{
		to_shift = determine_optimal_shift();

		// Leave a small buffer so we don't go completely dry
		if (cur_limit - to_shift < 2)
			to_shift = std::max(0, cur_limit - 1);

		send_packet = true;
	}

	return true;
}

void tickbase::revert_shift_back()
{
	to_shift = 0;
}

void tickbase::on_send_command(int command_number)
{
	to_adjust = 0;

	const auto wpn = local_weapon;
	auto& p1 = prediction::get_pred_info(command_number);

	if (p1.sequence != command_number)
		return;

	p1.tickbase.sent_commands = interfaces::client_state()->chokedcommands + 1;

	// Multi-feature priority: doubletap > hideshot > fakeduck
	const bool want_dt = fast_fire && vars::aim.doubletap->get<bool>();
	const bool want_hs = !want_dt && hide_shot && vars::aim.silent->get<bool>();
	const bool want_fd = vars::aim.fake_duck->get<bool>();

	// During peek fakelag we force skip adjust for clean DT / HS
	if ((want_dt || want_hs) && wpn && wpn->get_weapon_id() != WEAPON_REVOLVER &&
		antiaim::started_peek_fakelag() && !to_shift)
	{
		skip_next_adjust = true;
	}

	if (skip_next_adjust)
	{
		interfaces::prediction()->get_predicted_commands() =
			clamp(interfaces::client_state()->lastoutgoingcommand - interfaces::client_state()->last_command_ack,
				0, interfaces::prediction()->get_predicted_commands());
	}
	else
	{
		to_adjust = p1.tickbase.limit;
	}

	// Propagate skip flag to all commands in the batch
	for (auto i = interfaces::client_state()->lastoutgoingcommand + 1; i <= command_number; i++)
	{
		auto& p2 = prediction::get_pred_info(i);
		if (p2.sequence != i)
			continue;

		p2.tickbase.skip_fake_commands = skip_next_adjust;
	}

	// Keep limit tracking up to date
	compute_current_limit(command_number);
}

void tickbase::fill_fake_commands()
{
	const auto wpn = local_weapon;
	if (!wpn)
		return;

	const auto is_grenade = wpn->is_grenade();

	skip_next_adjust = false;
	for (auto i = 0; i < to_adjust; i++)
	{
		interfaces::client_state()->chokedcommands++;
		const auto sequence = interfaces::client_state()->lastoutgoingcommand + interfaces::client_state()->chokedcommands + 1;
		const auto cmd = &interfaces::input()->m_pCommands[sequence % 150];
		*cmd = *globals::current_cmd;
		cmd->command_number = sequence;
		if (!is_grenade)
			cmd->buttons &= ~(IN_ATTACK | IN_ATTACK2);
		cmd->tick_count = globals::current_cmd->tick_count + 200 + i;
		misc::write_tick(cmd->command_number);
	}
}

void tickbase::on_runcmd(const CUserCmd* cmd, int& tickbase)
{
	const auto& p1 = prediction::get_pred_info(cmd->command_number);
	if (p1.sequence != cmd->command_number)
		return;

	auto to_adjust = 0;
	std::optional<bool> prev_skip_fake_commands;

	for (auto i = interfaces::client_state()->last_command_ack; i <= cmd->command_number; i++)
	{
		const auto& p2 = prediction::get_pred_info(i);
		if (p2.sequence != i)
			continue;

		if (p2.tickbase.invalid_commands > 0)
		{
			prev_skip_fake_commands = false;
			continue;
		}

		if (!prev_skip_fake_commands.has_value())
			prev_skip_fake_commands = p2.tickbase.skip_fake_commands;

		if (prev_skip_fake_commands != p2.tickbase.skip_fake_commands)
			to_adjust = (p2.tickbase.skip_fake_commands ? p2.tickbase.limit : -p2.tickbase.limit) + p2.tickbase.adjust;
		else
			to_adjust = 0;

		prev_skip_fake_commands = p2.tickbase.skip_fake_commands;
	}

	tickbase += to_adjust;
}

void tickbase::on_recharge(int command_number)
{
	auto& p = prediction::get_pred_info(command_number);
	p.reset();
	p.sequence = command_number;
	p.tickbase.invalid_commands++;
}

void tickbase::on_finish_command(bool send_packet)
{
	const auto cmd = interfaces::client_state()->lastoutgoingcommand + interfaces::client_state()->chokedcommands + 1;
	auto& p = prediction::get_pred_info(cmd);
	if (p.sequence != cmd)
		return;

	if (to_shift > 0)
		p.tickbase.extra_commands++;

	if (send_packet)
		fill_fake_commands();
}

bool tickbase::apply_static_configuration()
{
	const auto previous = fast_fire || hide_shot;

	if (vars::aim.fake_duck->get<bool>())
		fast_fire = hide_shot = false;
	else
	{
		fast_fire = vars::aim.doubletap->get<bool>();
		hide_shot = !fast_fire && vars::aim.silent->get<bool>();
	}

	return previous != (fast_fire || hide_shot);
}

int tickbase::determine_optimal_shift()
{
	const auto wpn = local_weapon;
	if (!wpn)
		return 0;

	const auto info = interfaces::weapon_system()->GetWpnData(wpn->get_weapon_id());
	if (!info)
		return 0;

	const int max_limit = compute_current_limit();
	if (max_limit <= 0)
		return 0;

	// Base values
	constexpr int min_shift = 4;
	int desired = max_limit;

	// Dynamic adjustment based on ping
	const float rtt = misc::get_latency();          // your existing latency helper
	tick_interval = interfaces::globals()->interval_per_tick;
	const int ping_ticks = time_to_ticks(rtt);

	// On higher ping the server is more likely to clamp large shifts
	if (ping_ticks > 12)
		desired = std::min(desired, max_limit - 2);
	else if (ping_ticks > 7)
		desired = std::min(desired, max_limit - 1);

	// Weapon-aware shift
	if (wpn->is_secondary_attack_weapon() || wpn->get_weapon_id() == WEAPON_REVOLVER)
	{
		// Revolvers / secondary attack weapons need almost the full limit
		desired = max_limit;
	}
	else if (wpn->is_knife())
	{
		desired = std::min(desired, 7);
	}
	else
	{
		// Normal guns: try to leave a small buffer so we can still recharge
		const int cycle_ticks = time_to_ticks(info->cycle_time);
		desired = std::clamp(cycle_ticks - 1, min_shift, max_limit);
	}

	// Peek assist wants maximum shift
	if (vars::misc.peek_assist->get<bool>())
		desired = max_limit;

	// Lag-comp window consideration
	// Shifting forward by N ticks also extends the server's lag-comp window by N ticks.
	// We prefer a value that still gives good backtrack depth.
	const int safe_shift = std::clamp(desired, min_shift, max_limit);

	// Final safety clamp
	return std::clamp(safe_shift, min_shift, max_limit);
}

int tickbase::determine_optimal_limit()
{
	if (fast_fire || hide_shot)
		return max_new_cmds;

	return 0;
}

int tickbase::compute_current_limit(int command_number)
{
	if (!command_number)
		return 0;

	const auto& p = prediction::get_pred_info(interfaces::client_state()->last_command_ack);
	auto limit = p.sequence == interfaces::client_state()->last_command_ack ? p.tickbase.limit : 0;

	for (auto i = interfaces::client_state()->last_command_ack + 1; i <= command_number; i++)
	{
		auto& p2 = prediction::get_pred_info(i);
		if (p2.sequence != i)
			continue;

		p2.tickbase.limit = clamp(limit + p2.tickbase.invalid_commands, 0, sv_maxusrcmdprocessticks);
		p2.tickbase.limit = limit = std::max(p2.tickbase.limit - p2.tickbase.extra_commands, 0);
	}

	return limit;
}

float tickbase::get_adjusted_time()
{
	return ticks_to_time(local_player->get_tickbase() - 1);
}

bool tickbase::is_ready()
{
	return !to_recharge && !to_shift;
}
