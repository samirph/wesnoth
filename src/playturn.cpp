/* $Id$ */
/*
   Copyright (C) 2003 by David White <davidnwhite@verizon.net>
   Part of the Battle for Wesnoth Project http://www.wesnoth.org/

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License.
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY.

   See the COPYING file for more details.
*/

#include "playturn.hpp"

#include "game_config.hpp"
#include "gettext.hpp"
#include "replay.hpp"
#include "show_dialog.hpp"
#include "sound.hpp"

turn_info::turn_info(const game_data& gameinfo, game_state& state_of_game,
                     const gamestatus& status, display& gui, gamemap& map,
		     std::vector<team>& teams, unsigned int team_num, unit_map& units,
			 replay_network_sender& replay_sender, undo_list& undo_stack)
  : gameinfo_(gameinfo), state_of_game_(state_of_game), status_(status),
    gui_(gui), map_(map), teams_(teams), team_num_(team_num),
    units_(units), undo_stack_(undo_stack),
	replay_sender_(replay_sender), replay_error_("network_replay_error")
{}

turn_info::~turn_info(){
	undo_stack_.clear();
}

void turn_info::turn_slice()
{
	events::pump();
	events::raise_process_event();
	events::raise_draw_event();
}

void turn_info::sync_network()
{
	if(network::nconnections() > 0) {

		//receive data first, and then send data. When we sent the end of
		//the AI's turn, we don't want there to be any chance where we
		//could get data back pertaining to the next turn.
		config cfg;
		while(network::connection res = network::receive_data(cfg)) {
			std::deque<config> backlog;
			process_network_data(cfg,res,backlog,false);
			cfg.clear();
		}

		send_data();
	}
}

void turn_info::send_data()
{
	if(undo_stack_.empty()) {
		replay_sender_.commit_and_sync();
	} else {
		replay_sender_.sync_non_undoable();
	}
}

turn_info::PROCESS_DATA_RESULT turn_info::process_network_data(const config& cfg, network::connection from, std::deque<config>& backlog, bool skip_replay)
{
	if(cfg.child("whisper") != NULL && is_observer()){
		sound::play_sound(game_config::sounds::receive_message);

		const config& cwhisper = *cfg.child("whisper");
		gui_.add_chat_message("whisper: "+cwhisper["sender"],0,cwhisper["message"], display::MESSAGE_PRIVATE);
		}
	if(cfg.child("observer") != NULL) {
		const config::child_list& observers = cfg.get_children("observer");
		for(config::child_list::const_iterator ob = observers.begin(); ob != observers.end(); ++ob) {
			gui_.add_observer((**ob)["name"]);
		}
	}

	if(cfg.child("observer_quit") != NULL) {
		const config::child_list& observers = cfg.get_children("observer_quit");
		for(config::child_list::const_iterator ob = observers.begin(); ob != observers.end(); ++ob) {
			gui_.remove_observer((**ob)["name"]);
		}
	}

	if(cfg.child("leave_game") != NULL) {
		throw network::error("");
	}

	bool turn_end = false;

	const config::child_list& turns = cfg.get_children("turn");
	if(turns.empty() == false && from != network::null_connection) {
		//forward the data to other peers
		network::send_data_all_except(cfg,from);
	}

	for(config::child_list::const_iterator t = turns.begin(); t != turns.end(); ++t) {

		if(turn_end == false) {
			replay replay_obj(**t);
			replay_obj.set_skip(skip_replay);
			replay_obj.start_replay();

			try{
				turn_end = do_replay(gui_,map_,gameinfo_,units_,teams_,
				team_num_,status_,state_of_game_,&replay_obj);
			}
			catch (replay::error& e){
				//notify remote hosts of out of sync error
				config cfg;
				config& info = cfg.add_child("info");
				info["type"] = "termination";
				info["condition"] = "out of sync";
				network::send_data(cfg);

				replay::last_replay_error = e.message; //FIXME: some better way to pass this?
				replay_error_.notify_observers();
			}

			recorder.add_config(**t,replay::MARK_AS_SENT);
		} else {

			//this turn has finished, so push the remaining moves
			//into the backlog
			backlog.push_back(config());
			backlog.back().add_child("turn",**t);
		}
	}

	if(const config* change= cfg.child("change_controller")) {
		const int side = lexical_cast_default<int>((*change)["side"],1);
		const size_t index = static_cast<size_t>(side-1);

		const std::string& controller = (*change)["controller"];

		if(index < teams_.size()) {
			if(controller == "human") {
				teams_[index].make_human();
				gui_.set_team(index);
			} else if(controller == "network") {
				teams_[index].make_network();
			} else if(controller == "ai") {
				teams_[index].make_ai();
			}

			return PROCESS_RESTART_TURN;
		}
	}

	//if a side has dropped out of the game.
	if(cfg["side_drop"] != "") {
		const size_t side = atoi(cfg["side_drop"].c_str())-1;
		if(side >= teams_.size()) {
			LOG_STREAM(err, network) << "unknown side " << side << " is dropping game\n";
			throw network::error("");
		}

		int action = 0;

		std::vector<std::string> observers;

		//see if the side still has a leader alive. If they have
		//no leader, we assume they just want to be replaced by
		//the AI.
		const unit_map::const_iterator leader = find_leader(units_,side+1);
		if(leader != units_.end()) {
			std::vector<std::string> options;
			options.push_back(_("Replace with AI"));
			options.push_back(_("Replace with local player"));
			options.push_back(_("Abort game"));

			for(std::set<std::string>::const_iterator ob = gui_.observers().begin(); ob != gui_.observers().end(); ++ob) {
				options.push_back(_("Replace with ") + *ob);
				observers.push_back(*ob);
			}

			const std::string msg = leader->second.description() + " " + _("has left the game. What do you want to do?");
			action = gui::show_dialog2(gui_,NULL,"",msg,gui::OK_ONLY,&options);
		}

		//make the player an AI, and redo this turn, in case
		//it was the current player's team who has just changed into
		//an AI.
		switch(action) {
			case 0:
				teams_[side].make_ai();
				return PROCESS_RESTART_TURN;
			case 1:
				teams_[side].make_human();
				return PROCESS_RESTART_TURN;
			default:
				if (action > 2) {
					const size_t index = static_cast<size_t>(action - 3);
					if (index < observers.size()) {
						teams_[side].make_network();
						change_side_controller(cfg["side_drop"], observers[index], false /*not our own side*/);
					} else {
						teams_[side].make_ai();
					}
					return PROCESS_RESTART_TURN;
				}
				break;
		}
		throw network::error("");
	}

	return turn_end ? PROCESS_END_TURN : PROCESS_CONTINUE;
}

void turn_info::change_side_controller(const std::string& side, const std::string& player, bool own_side)
{
	config cfg;
	config& change = cfg.add_child("change_controller");
	change["side"] = side;
	change["player"] = player;

	if(own_side) {
		change["own_side"] = "yes";
	}

	network::send_data(cfg);
}
