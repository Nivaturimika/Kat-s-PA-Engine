#include "dcon_generated.hpp"
#include "system_state.hpp"
#include "ai.hpp"
#include "demographics.hpp"
#include "effects.hpp"
#include "gui_effect_tooltips.hpp"
#include "math_fns.hpp"
#include "military.hpp"
#include "politics.hpp"
#include "prng.hpp"
#include "province_templates.hpp"
#include "nations_templates.hpp"
#include "triggers.hpp"
#include "economy_factory.hpp"
#include "economy_estimations.hpp"
#include "ai_templates.hpp"
#include "pdqsort.h"
#include "ve.hpp"

namespace command {
	void execute_fabricate_cb(sys::state& state, dcon::nation_id source, dcon::nation_id target, dcon::cb_type_id type);
}

namespace ai {
	/* Additional (counting allies) offensiv strenght of a country, except the country itself */
	constexpr inline float additional_offensive_str_factor = 0.75f;
	constexpr inline float puppet_str_factor = 0.75f;
	constexpr inline float safety_factor = 0.75f;
	constexpr inline float ally_overestimate = 1.5f;

	/* Sphere weights */
	constexpr inline float sphere_already_in_our_sphere_factor = 0.5f;
	constexpr inline float sphere_primary_culture_factor = 100.f;
	constexpr inline float sphere_culture_group_factor = 75.f;
	constexpr inline float sphere_neighbor_factor = 1.5f;
	constexpr inline float sphere_unreachable_factor = 0.25f;
	constexpr inline float sphere_uncivilized_factor = 1.05f;
	constexpr inline float sphere_same_continent = 1.25f;
	constexpr inline float sphere_avoid_distracting_cultural_leader = 0.f;
	constexpr inline float sphere_wargoal_factor = 0.f;

	/* Aggression multiplier towards uncivilized nations */
	constexpr inline float aggression_towards_unciv = 4.5f;
	constexpr inline float aggression_towards_at_war = 1.1f;
	constexpr inline float aggression_towards_rival = 5.f;
	constexpr inline float aggression_towards_adjacent = 4.5f;
	constexpr inline float aggression_towards_culture_group = 0.25f;

	float average_army_strength(sys::state& state, dcon::army_id a) {
		float total = 0.f;
		float value = 0.f;
		for(auto const am : state.world.army_get_army_membership(a)) {
			value += am.get_regiment().get_strength();
			total += 1.f;
		}
		return total > 0.f ? value / total : 0.f;
	}
	float average_army_org(sys::state& state, dcon::army_id a) {
		float total = 0.f;
		float value = 0.f;
		for(auto const am : state.world.army_get_army_membership(a)) {
			value += am.get_regiment().get_org();
			total += 1.f;
		}
		return total > 0.f ? value / total : 0.f;
	}

	float estimate_total_value(sys::state& state, dcon::nation_id n, dcon::nation_id target) {
		auto v = 1.f;
		for(const auto pc : state.world.nation_get_province_ownership(target)) {
			auto is_accepted = nations::nation_accepts_culture(state, n, pc.get_province().get_dominant_culture());
			v *= 2.5f;
		}
		/* Calculate density of industry */
		auto const i_score = float(state.world.nation_get_industrial_score(target));
		auto const total_pop = state.world.nation_get_demographics(n, demographics::total);
		auto const density = total_pop > 0.f ? (i_score * 100000.f) / total_pop : 0.f;
		v *= std::max(0.01f, density);
		return v;
	}

	/*	Return self strength, without counting overlords OR puppets
		The evaluated strength is military-score x total-army-techs */
	float estimate_strength_self(sys::state& state, dcon::nation_id n) {
		auto tech_factor = 1.f;
		for(const auto t : state.world.in_technology) {
			if(state.world.nation_get_active_technologies(n, t)
			&& state.culture_definitions.tech_folders[t.get_folder_index()].category == culture::tech_category::army_techs) {
				tech_factor += 0.1f;
			}
		}
		return state.world.nation_get_military_score(n) * tech_factor;
	}

	/*	Estimates the strenght of a given nation - puppets count their overlord in the final calculation
		this is so like Belgium doesn't declare on Luxembourgh (puppet of germany) because lux by itself
		is weak, but it's overlord is not */
	float estimate_strength(sys::state& state, dcon::nation_id n) {
		if(auto ovr = state.world.nation_get_overlord_as_subject(n); ovr && state.world.overlord_get_ruler(ovr)) {
			return estimate_strength(state, state.world.overlord_get_ruler(ovr));
		}
		auto value = ai::estimate_strength_self(state, n);
		for(auto subj : state.world.nation_get_overlord_as_ruler(n)) {
			value += ai::estimate_strength_self(state, subj.get_subject());
		}
		return value;
	}

	float estimate_defensive_strength(sys::state& state, dcon::nation_id n) {
		auto const cap = state.world.nation_get_capital(n);
		auto const continent = state.world.province_get_continent(cap);

		auto value = estimate_strength(state, n);
		for(auto dr : state.world.nation_get_diplomatic_relation(n)) {
			if(dr.get_are_allied()) {
				auto other = dr.get_related_nations(0) != n ? dr.get_related_nations(0) : dr.get_related_nations(1);
				if(other.get_overlord_as_subject().get_ruler() != n) {
					auto const ally_str = estimate_strength(state, other);
					value += ally_str;
				}
			}
		}
		return value;
	}

	float estimate_additional_offensive_strength(sys::state& state, dcon::nation_id n, dcon::nation_id target) {
		auto value = 0.f;
		for(auto dr : state.world.nation_get_diplomatic_relation(n)) {
			if(dr.get_are_allied()) {
				auto other = dr.get_related_nations(0) != n ? dr.get_related_nations(0) : dr.get_related_nations(1);
				if(other.get_overlord_as_subject().get_ruler() != n
				&& !military::has_truce_with(state, other, target)
				&& military::can_use_cb_against(state, other, target)) {
					auto const ally_str = estimate_strength(state, other);
					value += ally_str;
				}
			}
		}
		return value * additional_offensive_str_factor;
	}

	float war_weight_potential_target(sys::state& state, dcon::nation_id n, dcon::nation_id target, float base_strength) {
		auto const our_str = base_strength + estimate_additional_offensive_strength(state, n, target);
		auto const their_str = estimate_defensive_strength(state, target)
			- (nations::are_allied(state, target, n) ? base_strength : 0.f);
		
		auto weight = our_str - their_str;
		if(!state.world.nation_get_is_civilized(target)) {
			weight = our_str - (their_str * 0.25f);
			weight *= state.defines.aggression_unciv_bonus;
		}
		if(state.world.nation_get_is_at_war(target)) {
			weight *= aggression_towards_at_war;
		}
		if(state.world.nation_get_ai_rival(n) == target) {
			weight *= aggression_towards_rival;
		}
		auto const pc = state.world.nation_get_primary_culture(n);
		auto const cg = pc.get_group_from_culture_group_membership();
		auto const t_pc = state.world.nation_get_primary_culture(target);
		auto const t_cg = t_pc.get_group_from_culture_group_membership();
		if(cg == t_cg) {
			weight *= aggression_towards_culture_group;
		}

		auto const adj = state.world.get_nation_adjacency_by_nation_adjacency_pair(n, target);
		if(adj) {
			weight *= aggression_towards_adjacent;
		}
		auto const total_pop = state.world.nation_get_demographics(target, demographics::total);
		auto const pop_weight = (1.f / 100000.f); // each 100k
		return weight + total_pop * pop_weight;
	}

	//
	// UPDATE FUNCTIONS
	//

	void update_ai_general_status(sys::state& state) {
		for(auto n : state.world.in_nation) {
			if(n.get_owned_province_count() == 0
			|| n.get_is_player_controlled()) {
				n.set_ai_is_threatened(false);
				n.set_ai_rival(dcon::nation_id{});
				continue;
			}
			//auto ll = state.world.nation_get_last_war_loss(n);
			auto const cg = n.get_primary_culture().get_group_from_culture_group_membership();
			auto const cu = cg.get_identity_from_cultural_union_of();
			/**/
			dcon::nation_id rival_n{};
			auto rival_str = 0.0f;
			for(auto b : state.world.nation_get_nation_adjacency_as_connected_nations(n)) {
				auto other = b.get_connected_nations(b.get_connected_nations(0) == n ? 1 : 0);
				if(nations::are_allied(state, n, other))
					continue;
				if(other.get_in_sphere_of()
				&& (n.get_in_sphere_of() == other.get_in_sphere_of()
				|| n.get_in_sphere_of() == other.get_in_sphere_of()))
					continue;
				if(cg == other.get_primary_culture().get_group_from_culture_group_membership())
					continue;
				/* Extra weights to influence formation of cultural unions */
				auto other_str = estimate_strength(state, other);
				for(auto const po : other.get_province_ownership()) {
					if(province::has_core(state, po.get_province(), cu)
					|| province::has_core(state, po.get_province(), n.get_identity_from_identity_holder())) {
						other_str *= 10.f; /* x10 each province with a CU core */
					}
				}
				if(auto ov = other.get_overlord_as_subject(); ov.get_ruler()) {
					other = ov.get_ruler();
					if(other == n) {
						continue;
					}
				}
				if(rival_str < other_str) {
					rival_str = other_str;
					rival_n = other;
				}
			}
			if(rival_n) {
				auto const defensive_str = estimate_defensive_strength(state, n);
				auto const threatened = defensive_str < safety_factor * rival_str;
				state.world.nation_set_ai_is_threatened(n, threatened);
				state.world.nation_set_ai_rival(n, rival_n);
			}
		}
	}

	static void internal_get_alliance_targets_by_adjacency(sys::state& state, dcon::nation_id n, dcon::nation_id adj, std::vector<dcon::nation_id, dcon::cache_aligned_allocator<dcon::nation_id>>& alliance_targets) {
		for(auto nb : state.world.nation_get_nation_adjacency(adj)) {
			auto other = nb.get_connected_nations(0) != adj ? nb.get_connected_nations(0) : nb.get_connected_nations(1);
			bool b = other.get_is_player_controlled() ? true : ai_will_accept_alliance(state, other, n);
			if(other != n && !(other.get_overlord_as_subject().get_ruler()) && !nations::are_allied(state, n, other) && !military::are_at_war(state, other, n) && b) {
				alliance_targets.push_back(other.id);
			}
		}
	}

	static void internal_get_alliance_targets(sys::state& state, dcon::nation_id n, std::vector<dcon::nation_id, dcon::cache_aligned_allocator<dcon::nation_id>>& alliance_targets) {
		// Adjacency with us
		internal_get_alliance_targets_by_adjacency(state, n, n, alliance_targets);
		if(!alliance_targets.empty())
			return;

		// Adjacency with rival (useful for e.x, Chile allying Paraguay to fight bolivia)
		if(auto rival = state.world.nation_get_ai_rival(n); bool(rival)) {
			internal_get_alliance_targets_by_adjacency(state, n, rival, alliance_targets);
			if(!alliance_targets.empty())
				return;
		}

		// Adjacency with people who are at war with us
		for(auto wp : state.world.nation_get_war_participant(n)) {
			for(auto p : state.world.war_get_war_participant(wp.get_war())) {
				if(p.get_is_attacker() == !wp.get_is_attacker()) {
					internal_get_alliance_targets_by_adjacency(state, n, p.get_nation(), alliance_targets);
					if(!alliance_targets.empty())
						return;
				}
			}
		}
	}

	void form_alliances(sys::state& state) {
		auto ymd = state.current_date.to_ymd(state.start_date);
		for(auto n : state.world.in_nation) {
			if(!n.get_is_player_controlled() && n.get_ai_is_threatened() && !(n.get_overlord_as_subject().get_ruler())) {
				static std::vector<dcon::nation_id, dcon::cache_aligned_allocator<dcon::nation_id>> alliance_targets;
				alliance_targets.clear();
				internal_get_alliance_targets(state, n, alliance_targets);
				if(!alliance_targets.empty()) {
					pdqsort(alliance_targets.begin(), alliance_targets.end(), [&](dcon::nation_id a, dcon::nation_id b) {
						auto a_str = estimate_strength(state, a);
						auto b_str = estimate_strength(state, b);
						if(a_str != b_str)
							return a_str > b_str;
						return a.index() > b.index();
					});
					if(state.world.nation_get_is_player_controlled(alliance_targets[0])) {
						if(ymd.day == 1 && ymd.month == 1) {
							assert(command::can_ask_for_alliance(state, n, alliance_targets[0]));
							command::execute_ask_for_alliance(state, n, alliance_targets[0]);
						}
					} else {
						nations::make_alliance(state, n, alliance_targets[0]);
					}
				}
			}
		}
	}

	void prune_alliances(sys::state& state) {
		for(auto n : state.world.in_nation) {
			if(!n.get_is_player_controlled()
			&& !n.get_ai_is_threatened()
			&& !(n.get_overlord_as_subject().get_ruler())
			&& n.get_diplomatic_points() >= state.defines.cancelalliance_diplomatic_cost) {
				// Don't break alliances if we recently lost a war
				auto const defensive_str = estimate_defensive_strength(state, n);
				auto const ll = state.world.nation_get_last_war_loss(n);
				if(ll == sys::date{} || ll + 365 >= state.current_date) {
					auto greatest_neighbor = 0.0f;
					auto const in_sphere_of = state.world.nation_get_in_sphere_of(n);
					for(auto const b : state.world.nation_get_nation_adjacency_as_connected_nations(n)) {
						auto const other = b.get_connected_nations(0) != n ? b.get_connected_nations(0) : b.get_connected_nations(1);
						auto const in_same_sphere = in_sphere_of == n
							|| (in_sphere_of && in_sphere_of == other.get_in_sphere_of());
						if(!nations::are_allied(state, n, other)
						&& !in_same_sphere) {
							greatest_neighbor = std::max(greatest_neighbor, estimate_strength(state, other));
						}
					}
					static std::vector<dcon::nation_id, dcon::cache_aligned_allocator<dcon::nation_id>> prune_targets;
					prune_targets.clear();
					for(auto dr : n.get_diplomatic_relation()) {
						if(dr.get_are_allied()) {
							auto other = dr.get_related_nations(0) != n ? dr.get_related_nations(0) : dr.get_related_nations(1);
							if(n.get_in_sphere_of() != other
							&& other.get_in_sphere_of() != n) {
								prune_targets.push_back(other);
							}
						}
					}
					pdqsort(prune_targets.begin(), prune_targets.end(), [&](dcon::nation_id a, dcon::nation_id b) {
						auto const a_str = estimate_strength(state, a);
						auto const b_str = estimate_strength(state, b);
						if(a_str != b_str)
							return a_str > b_str;
						return a.index() > b.index();
					});

					auto safety_margin = defensive_str - safety_factor * greatest_neighbor;
					for(auto const t : prune_targets) {
						auto const weakest_str = estimate_strength(state, t);
						if(weakest_str >= safety_margin) {
							break;
						}
						safety_margin -= weakest_str;
						assert(command::can_cancel_alliance(state, n, t));
						command::execute_cancel_alliance(state, n, t);
					}
				}
			}
		}
	}

	bool ai_is_close_enough(sys::state& state, dcon::nation_id target, dcon::nation_id from) {
		auto target_continent = state.world.province_get_continent(state.world.nation_get_capital(target));
		auto source_continent = state.world.province_get_continent(state.world.nation_get_capital(from));
		return (target_continent == source_continent) || bool(state.world.get_nation_adjacency_by_nation_adjacency_pair(target, from));
	}

	static bool ai_has_mutual_enemy(sys::state& state, dcon::nation_id from, dcon::nation_id target) {
		auto rival_a = state.world.nation_get_ai_rival(target);
		auto rival_b = state.world.nation_get_ai_rival(from);
		// Same rival equates to instantaneous alliance (we benefit from more allies against a common enemy)
		if(rival_a && rival_a == rival_b) {
			return true;
		}
		// Our rivals are allied?
		if(rival_a && rival_b && rival_a != rival_b && nations::are_allied(state, rival_a, rival_b)) {
			return true;
		}
		// // One of the allies of our rivals can be declared on?
		for(auto n : state.world.in_nation) {
			if(n.id != target && n.id != from && n.id != rival_a && n.id != rival_b) {
				if(nations::are_allied(state, rival_a, n.id) || nations::are_allied(state, rival_b, n.id)) {
					bool enemy_a = military::can_use_cb_against(state, from, n.id);
					bool enemy_b = military::can_use_cb_against(state, target, n.id);
					if(enemy_a || enemy_b) {
						return true;
					}
				}
			}
		}
		return false;
	}

	bool ai_will_accept_alliance(sys::state& state, dcon::nation_id target, dcon::nation_id from) {
		// Puppets always ally overlords
		auto ovr = state.world.nation_get_overlord_as_subject(target);
		if(ovr && state.world.overlord_get_ruler(ovr) == from)
			return true;

		if(nations::has_core_in_nation(state, state.world.nation_get_identity_from_identity_holder(target), from))
			return false;

		/*auto pc = state.world.nation_get_primary_culture(target);
		auto cg = state.world.culture_get_group_from_culture_group_membership(pc);
		auto cu = state.world.culture_group_get_identity_from_cultural_union_of(cg);
		if(cu) {
			if(nations::has_core_in_nation(state, cu, from))
			return false;
		}*/

		if(!state.world.nation_get_ai_is_threatened(target))
			return false;

		// Has not surpassed infamy limit
		if(state.world.nation_get_infamy(target) >= state.defines.badboy_limit)
			return false;

		// Won't ally our rivals
		if(state.world.nation_get_ai_rival(target) == from || state.world.nation_get_ai_rival(from) == target)
			return false;

		//until great wars
		if(state.world.nation_get_is_great_power(target) && !state.military_definitions.great_wars_enabled) {
			int32_t gp_count = 0;
			for(const auto rel : state.world.nation_get_diplomatic_relation(from)) {
				auto n = rel.get_related_nations(rel.get_related_nations(0) == from ? 1 : 0);
				if(rel.get_are_allied() && n.get_is_great_power()) {
					if(gp_count >= 2) {
						return false;
					}
					++gp_count;
				}
			}
		}

		// AI's ignore relations with other AI's, but with player
		auto rel = state.world.get_diplomatic_relation_by_diplomatic_pair(target, from);
		if(state.world.diplomatic_relation_get_value(rel) >= state.defines.relation_limit_no_alliance_offer && state.world.nation_get_is_player_controlled(from))
			return false;

		auto spherelord = state.world.nation_get_in_sphere_of(from);
		if(spherelord) {
			//If no spherelord -> Then must not ally spherelings
			//If spherelord -> Then must not ally non-spherelings
			if(state.world.nation_get_in_sphere_of(target) != spherelord && target != spherelord)
				return false;
			if(target == spherelord)
				return true; //always ally spherelord
		}

		if(ai_has_mutual_enemy(state, from, target))
			return true;
		// Otherwise we may consider alliances only iff they are close to our continent or we are adjacent
		if(!ai_is_close_enough(state, target, from))
			return false;
		// And also if they're powerful enough to be considered for an alliance
		auto target_score = estimate_strength(state, target);
		auto source_score = estimate_strength(state, from);
		return std::max<float>(source_score, 1.f) * ally_overestimate >= target_score;
	}

	void explain_ai_alliance_reasons(sys::state& state, dcon::nation_id target, text::layout_base& contents, int32_t indent) {
		text::add_line_with_condition(state, contents, "ai_alliance_1", state.world.nation_get_ai_is_threatened(target), indent);
		text::add_line(state, contents, "any_of_the_following", indent);
		text::add_line_with_condition(state, contents, "ai_alliance_5", ai_has_mutual_enemy(state, state.local_player_nation, target), indent + 15);
		text::add_line(state, contents, "any_of_the_following", indent);
		text::add_line_with_condition(state, contents, "ai_alliance_2", ai_is_close_enough(state, target, state.local_player_nation), indent + 15);
		text::add_line_with_condition(state, contents, "ai_alliance_3", state.world.nation_get_ai_rival(target) != state.local_player_nation && state.world.nation_get_ai_rival(state.local_player_nation) != target, indent + 15);
		auto target_score = estimate_strength(state, target);
		auto source_score = estimate_strength(state, state.local_player_nation);
		text::add_line_with_condition(state, contents, "ai_alliance_4", std::max<float>(source_score, 1.f) * ally_overestimate >= target_score, indent + 15);
		text::add_line_with_condition(state, contents, "ai_alliance_6", nations::has_core_in_nation(state, state.world.nation_get_identity_from_identity_holder(target), state.local_player_nation));
	}

	bool ai_will_grant_access(sys::state& state, dcon::nation_id target, dcon::nation_id from) {
		if(!state.world.nation_get_is_at_war(from))
			return false;
		if(state.world.nation_get_ai_rival(target) == from)
			return false;
		if(military::are_at_war(state, from, state.world.nation_get_ai_rival(target)))
			return true;
		for(auto wa : state.world.nation_get_war_participant(target)) {
			auto is_attacker = wa.get_is_attacker();
			for(auto o : wa.get_war().get_war_participant()) {
				if(o.get_is_attacker() != is_attacker) {
					if(military::are_at_war(state, o.get_nation(), from))
						return true;
				}
			}
		}
		return false;

	}
	void explain_ai_access_reasons(sys::state& state, dcon::nation_id target, text::layout_base& contents, int32_t indent) {
		text::add_line_with_condition(state, contents, "ai_access_1", ai_will_grant_access(state, target, state.local_player_nation), indent);
	}

	void update_ai_research(sys::state& state) {
		auto ymd_date = state.current_date.to_ymd(state.start_date);
		auto year = uint32_t(ymd_date.year);
		concurrency::parallel_for(uint32_t(0), state.world.nation_size(), [&](uint32_t id) {
			dcon::nation_id n{ dcon::nation_id::value_base_t(id) };
			if(state.world.nation_get_is_player_controlled(n)
			|| state.world.nation_get_current_research(n)
			|| !state.world.nation_get_is_civilized(n)
			|| state.world.nation_get_owned_province_count(n) == 0) {
				//skip -- does not need new research
				return;
			}

			struct potential_techs {
				dcon::technology_id id;
				float weight = 0.0f;
			};
			std::vector<potential_techs, dcon::cache_aligned_allocator<potential_techs>> potential;

			for(auto tid : state.world.in_technology) {
				if(state.world.nation_get_active_technologies(n, tid)) {
					continue; // Already researched
				}

				if(state.current_date.to_ymd(state.start_date).year >= state.world.technology_get_year(tid)) {
					// Find previous technology before this one
					dcon::technology_id prev_tech = dcon::technology_id(dcon::technology_id::value_base_t(tid.id.index() - 1));
					// Previous technology is from the same folder so we have to check that we have researched it beforehand
					if(tid.id.index() != 0 && state.world.technology_get_folder_index(prev_tech) == state.world.technology_get_folder_index(tid)) {
						// Only allow if all previously researched techs are researched
						if(state.world.nation_get_active_technologies(n, prev_tech)) {
							potential.push_back(potential_techs{ tid, 0.0f });
						}
					} else { // first tech in folder
						potential.push_back(potential_techs{ tid, 0.0f });
					}
				}
			}

			for(auto& pt : potential) { // weight techs
				auto k_mod = state.world.technology_get_ai_chance(pt.id);
				auto base = k_mod ? trigger::evaluate_multiplicative_modifier(state, k_mod, trigger::to_generic(n), trigger::to_generic(n), -1) : 0.f;
				//if(state.world.nation_get_ai_is_threatened(n) && state.culture_definitions.tech_folders[state.world.technology_get_folder_index(pt.id)].category == culture::tech_category::army) {
				//	base *= 2.0f;
				//}
				auto cost = std::max(1.0f, culture::effective_technology_cost(state, year, n, pt.id));
				pt.weight = base / cost;
			}
			pdqsort(potential.begin(), potential.end(), [&](potential_techs& a, potential_techs& b) {
				if(a.weight != b.weight)
					return a.weight > b.weight;
				return a.id.index() > b.id.index();
			});
			if(!potential.empty()) {
				state.world.nation_set_current_research(n, potential[0].id);
			}
		});
	}

	/* Tests if a nation is a potential candidate to go to war with, taking into account if it is in a sphere
		@param n Nation (spherelord)
		@param t Target to test for
		@return If the target is viable to go to war with */
	bool target_in_sphere_potential_war_target(sys::state& state, dcon::nation_id n, dcon::nation_id t) {
		if((state.world.nation_get_infamy(n) + state.defines.removefromsphere_infamy_cost) >= state.defines.badboy_limit) {
			return false; //do not go over infamy limit
		}
		auto rel = state.world.get_gp_relationship_by_gp_influence_pair(t, n);
		assert(state.world.nation_get_in_sphere_of(t) == n);
		return rel && military::can_use_cb_against(state, n, t);
	}

	void update_influence_priorities(sys::state& state) {
		struct weighted_nation {
			dcon::nation_id id;
			float weight = 0.0f;
		};

		for(auto gprl : state.world.in_gp_relationship) {
			if(gprl.get_great_power().get_is_player_controlled()) {
				// nothing -- player GP
			} else {
				auto& status = gprl.get_status();
				status &= ~nations::influence::priority_mask;
				if((status & nations::influence::level_mask) == nations::influence::level_in_sphere) {
					status |= nations::influence::priority_zero;
				}
			}
		}

		for(auto& n : state.great_nations) {
			if(state.world.nation_get_is_player_controlled(n.nation))
				continue;

			static std::vector<weighted_nation, dcon::cache_aligned_allocator<weighted_nation>> targets;
			targets.clear();
			for(auto t : state.world.in_nation) {
				if(t.get_is_great_power()
				|| t.get_owned_province_count() == 0)
					continue;
				// Avoid sphereing big states like china
				if(t.get_demographics(demographics::total) > state.defines.large_population_limit) {
					auto const pop_factor = state.defines.large_population_influence_penalty * t.get_demographics(demographics::total) / state.defines.large_population_influence_penalty_chunk;
					if(pop_factor < -1.f) {
						continue;
					}
				}
				auto rel = state.world.get_gp_relationship_by_gp_influence_pair(t, n.nation);
				if(!nations::can_accumulate_influence_with(state, n.nation, t, rel)) {
					continue;
				}
				auto weight = 1.f;
				for(auto const c : state.world.in_commodity) {
					if(auto const d = state.world.nation_get_real_demand(n.nation, c); d > 0.1f) {
						auto const cweight = std::min(1.0f, t.get_domestic_market_pool(c) / d) * (1.0f - state.world.nation_get_demand_satisfaction(n.nation, c));
						weight += std::max(1.f, cweight);
					}
				}
				//We probably don't want to fight a forever lasting sphere war, let's find some other uncontested nations
				auto const pc = state.world.nation_get_primary_culture(n.nation);
				auto const cg = pc.get_group_from_culture_group_membership();
				auto const t_pc = t.get_primary_culture();
				auto const t_cg = t.get_primary_culture().get_group_from_culture_group_membership();
				if(t.get_in_sphere_of() == n.nation) {
					weight *= sphere_already_in_our_sphere_factor;
				} else {
					auto const our_rank = state.world.nation_get_rank(n.nation);
					bool cg_leader_influencing = false;
					for(const auto gp : t.get_gp_relationship_as_influence_target()) {
						if(gp.get_great_power().get_primary_culture().get_group_from_culture_group_membership() == cg
						&& gp.get_great_power().get_rank() < our_rank) {
							cg_leader_influencing = true;
						}
					}
					/* The "cultural union leader" is already influencing this -- so don't bother them */
					if(cg_leader_influencing) {
						weight  *= sphere_avoid_distracting_cultural_leader;
					} else {
						//Prioritize primary culture before culture groups; should ensure Prussia spheres all of the NGF first before trying to contest Austria
						if(t_pc == pc) {
							weight *= sphere_primary_culture_factor;
						}
						if(t_cg == cg
						|| nations::has_core_in_nation(state, cg.get_identity_from_cultural_union_of(), t)) {
							weight *= sphere_culture_group_factor;
						}
						//If it doesn't neighbor us or a friendly sphere and isn't coastal, please don't sphere it, we don't want sphere gore
						bool is_reachable = false;
						for(auto adj : state.world.nation_get_nation_adjacency(t)) {
							auto n_adj = adj.get_connected_nations(0) != t ? adj.get_connected_nations(0) : adj.get_connected_nations(1);
							if(n_adj == n.nation
							|| n_adj.get_in_sphere_of() == n.nation) {
								is_reachable = true;
								break;
							}
						};
						//Is coastal? Technically reachable
						if(state.world.nation_get_central_ports(t) > 0) {
							is_reachable = true;
						}
						//Prefer neighbors
						if(state.world.get_nation_adjacency_by_nation_adjacency_pair(n.nation, t.id)) {
							weight *= sphere_neighbor_factor;
							is_reachable = true;
						}
						if(is_reachable) {
							// In same continent
							if(t.get_capital().get_continent() == state.world.province_get_continent(state.world.nation_get_capital(n.nation))) {
								weight *= sphere_same_continent;
							}
							if(!t.get_is_civilized()) {
								weight *= sphere_uncivilized_factor;
							}
						} else {
							weight *= sphere_unreachable_factor;
						}
					}
					if(military::can_use_cb_against(state, n.nation, t)) {
						weight *= sphere_wargoal_factor;
					}
				}
				if(weight > 0.f) {
					targets.push_back(weighted_nation{ t.id, weight });
				}
			}

			pdqsort(targets.begin(), targets.end(), [](weighted_nation const& a, weighted_nation const& b) {
				if(a.weight != b.weight)
					return a.weight > b.weight;
				return a.id.index() < b.id.index();
			});

			uint32_t i = 0;
			for(; i < 1 && i < targets.size(); ++i) {
				auto rel = state.world.get_gp_relationship_by_gp_influence_pair(targets[i].id, n.nation);
				if(!rel)
					rel = state.world.force_create_gp_relationship(targets[i].id, n.nation);
				state.world.gp_relationship_get_status(rel) |= nations::influence::priority_three;
			}
			for(; i < 2 && i < targets.size(); ++i) {
				auto rel = state.world.get_gp_relationship_by_gp_influence_pair(targets[i].id, n.nation);
				if(!rel)
					rel = state.world.force_create_gp_relationship(targets[i].id, n.nation);
				state.world.gp_relationship_get_status(rel) |= nations::influence::priority_two;
			}
			for(; i < 4 && i < targets.size(); ++i) {
				auto rel = state.world.get_gp_relationship_by_gp_influence_pair(targets[i].id, n.nation);
				if(!rel)
					rel = state.world.force_create_gp_relationship(targets[i].id, n.nation);
				state.world.gp_relationship_get_status(rel) |= nations::influence::priority_one;
			}
		}
	}

	void perform_influence_actions(sys::state& state) {
		for(auto gprl : state.world.in_gp_relationship) {
			if(gprl.get_great_power().get_is_player_controlled()) {
				// nothing -- player GP
			} else {
				if((gprl.get_status() & nations::influence::is_banned) != 0) {
					continue; // can't do anything with a banned nation
				}
				if(military::are_at_war(state, gprl.get_great_power(), gprl.get_influence_target())) {
					continue; // can't do anything while at war
				}

				auto current_sphere = gprl.get_influence_target().get_in_sphere_of();
				auto clevel = (nations::influence::level_mask & gprl.get_status());
				if(clevel == nations::influence::level_in_sphere) {
					//De-sphere countries we have wargoals against, desphering countries need to check for going over infamy
					if(current_sphere == gprl.get_great_power()
					&& ai::target_in_sphere_potential_war_target(state, gprl.get_great_power(), gprl.get_influence_target())) {
						assert(command::can_remove_from_sphere(state, gprl.get_great_power(), gprl.get_influence_target(), gprl.get_great_power()));
						command::execute_remove_from_sphere(state, gprl.get_great_power(), gprl.get_influence_target(), gprl.get_great_power());
						continue; // already in sphere, but now removed :)
					}
					// annoy the shit out of other gps that are messing with our spheres
					// so prussia wants to fucking murder austria
					for(auto other_gprl : gprl.get_influence_target().get_gp_relationship_as_influence_target()) {
						if(current_sphere && other_gprl.get_great_power() != gprl.get_great_power()) {
							// Ban first, then decrease after
							if((other_gprl.get_status() & nations::influence::is_banned) != 0
							&& command::can_decrease_opinion(state, gprl.get_great_power(), gprl.get_influence_target(), other_gprl.get_great_power())) {
								command::decrease_opinion(state, gprl.get_great_power(), gprl.get_influence_target(), other_gprl.get_great_power());
							} else if(command::can_ban_embassy(state, gprl.get_great_power(), gprl.get_influence_target(), other_gprl.get_great_power())) {
								command::ban_embassy(state, gprl.get_great_power(), gprl.get_influence_target(), other_gprl.get_great_power());
							}
						}
					}
					continue; // already in sphere
				}
				if(state.defines.increaseopinion_influence_cost <= gprl.get_influence() && clevel != nations::influence::level_friendly) {
					assert(command::can_increase_opinion(state, gprl.get_great_power(), gprl.get_influence_target()));
					command::execute_increase_opinion(state, gprl.get_great_power(), gprl.get_influence_target());
				} else if(state.defines.removefromsphere_influence_cost <= gprl.get_influence() && current_sphere && current_sphere != gprl.get_great_power() && clevel == nations::influence::level_friendly) { // condition taken care of by check above
					assert(command::can_remove_from_sphere(state, gprl.get_great_power(), gprl.get_influence_target(), gprl.get_influence_target().get_in_sphere_of()));
					command::execute_remove_from_sphere(state, gprl.get_great_power(), gprl.get_influence_target(), gprl.get_influence_target().get_in_sphere_of());
				} else if(state.defines.addtosphere_influence_cost <= gprl.get_influence() && !current_sphere && clevel == nations::influence::level_friendly) {
					assert(command::can_add_to_sphere(state, gprl.get_great_power(), gprl.get_influence_target()));
					command::execute_add_to_sphere(state, gprl.get_great_power(), gprl.get_influence_target());
				}
			}
		}
	}

	void identify_focuses(sys::state& state) {
		for(auto f : state.world.in_national_focus) {
			if(f.get_promotion_amount() > 0) {
				if(f.get_promotion_type() == state.culture_definitions.clergy)
					state.national_definitions.clergy_focus = f;
				if(f.get_promotion_type() == state.culture_definitions.soldiers)
					state.national_definitions.soldier_focus = f;
				if(f.get_promotion_type() == state.culture_definitions.aristocrat)
					state.national_definitions.aristocrat_focus = f;
				if(f.get_promotion_type() == state.culture_definitions.capitalists)
					state.national_definitions.capitalist_focus = f;
				if(f.get_promotion_type() == state.culture_definitions.primary_factory_worker)
					state.national_definitions.primary_factory_worker_focus = f;
				if(f.get_promotion_type() == state.culture_definitions.secondary_factory_worker)
					state.national_definitions.secondary_factory_worker_focus = f;
			}
		}
	}

	void update_focuses(sys::state& state) {
		for(auto si : state.world.in_state_instance) {
			if(!si.get_nation_from_state_ownership().get_is_player_controlled()) {
				si.set_owner_focus(dcon::national_focus_id{});
			}
		}

		for(auto n : state.world.in_nation) {
			if(n.get_is_player_controlled())
				continue;
			if(n.get_owned_province_count() == 0)
				continue;

			n.set_state_from_flashpoint_focus(dcon::state_instance_id{});

			auto num_focuses_total = nations::max_national_focuses(state, n);
			if(num_focuses_total <= 0)
				return;

			auto base_opt = state.world.pop_type_get_research_optimum(state.culture_definitions.clergy);
			auto clergy_frac = n.get_demographics(demographics::to_key(state, state.culture_definitions.clergy)) / n.get_demographics(demographics::total);
			bool max_clergy = clergy_frac >= base_opt;

			static std::vector<dcon::state_instance_id, dcon::cache_aligned_allocator<dcon::state_instance_id>> ordered_states;
			ordered_states.clear();
			for(auto si : n.get_state_ownership()) {
				auto total = si.get_state().get_demographics(demographics::total);
				if(total > 0.f) {
					ordered_states.push_back(si.get_state().id);
				}
			}
			pdqsort(ordered_states.begin(), ordered_states.end(), [&](auto a, auto b) {
				auto const a_prov = state.world.state_instance_get_capital(a);
				auto const b_prov = state.world.state_instance_get_capital(b);
				auto const a_colonial = state.world.province_get_is_colonial(a_prov);
				auto const b_colonial = state.world.province_get_is_colonial(b_prov);
				if(a_colonial != b_colonial)
					return a_colonial < b_colonial;
				auto const a_pop = state.world.state_instance_get_demographics(a, demographics::total);
				auto const b_pop = state.world.state_instance_get_demographics(b, demographics::total);
				if(a_pop != b_pop)
					return a_pop > b_pop;
				return a.index() < b.index();
			});
			for(uint32_t i = 0; num_focuses_total > 0 && i < ordered_states.size(); ++i) {
				auto const prov = state.world.state_instance_get_capital(ordered_states[i]);
				auto const total = state.world.state_instance_get_demographics(ordered_states[i], demographics::total);
				auto const cfrac = state.world.state_instance_get_demographics(ordered_states[i], demographics::to_key(state, state.culture_definitions.clergy)) / total;
				auto const is_colonial = state.world.province_get_is_colonial(prov);
				if(cfrac >= base_opt) {
					if(n.get_is_at_war() || is_colonial) {
						auto nf = state.national_definitions.soldier_focus;
						auto k = state.world.national_focus_get_limit(nf);
						if(!k || trigger::evaluate(state, k, trigger::to_generic(prov), trigger::to_generic(n), -1)) {
							assert(command::can_set_national_focus(state, n, ordered_states[i], nf));
							state.world.state_instance_set_owner_focus(ordered_states[i], state.national_definitions.soldier_focus);
							--num_focuses_total;
						}
					} else {
						auto const has_factories = economy_factory::state_built_factory_count(state, ordered_states[i]);
						auto const employable = state.world.state_instance_get_demographics(ordered_states[i], demographics::employable);
						auto const employed = state.world.state_instance_get_demographics(ordered_states[i], demographics::employed);
						auto const emp_ratio = employed > 0.f ? (employed / employable) : 0.f;
						if(has_factories && emp_ratio <= 0.95f) {
							auto nf = state.national_definitions.primary_factory_worker_focus;
							auto k = state.world.national_focus_get_limit(nf);
							if(!k || trigger::evaluate(state, k, trigger::to_generic(prov), trigger::to_generic(n), -1)) {
								// Keep balance between ratio of factory workers
								// we will only promote primary workers if none are unemployed
								assert(command::can_set_national_focus(state, n, ordered_states[i], nf));
								state.world.state_instance_set_owner_focus(ordered_states[i], nf);
								--num_focuses_total;
							}
						}
					}
				} else { //cfrac < base_opt
					// If we haven't maxxed out clergy on this state, then our number 1 priority is to maximize clergy
					auto nf = state.national_definitions.clergy_focus;
					auto k = state.world.national_focus_get_limit(nf);
					if(!k || trigger::evaluate(state, k, trigger::to_generic(prov), trigger::to_generic(n), -1)) {
						assert(command::can_set_national_focus(state, n, ordered_states[i], nf));
						state.world.state_instance_set_owner_focus(ordered_states[i], state.national_definitions.clergy_focus);
						--num_focuses_total;
					}
				}
			}
		}
	}

	void take_ai_decisions(sys::state& state) {
		using decision_nation_pair = std::pair<dcon::decision_id, dcon::nation_id>;
		concurrency::combinable<std::vector<decision_nation_pair, dcon::cache_aligned_allocator<decision_nation_pair>>> decisions_taken;

		// execute in staggered blocks
		uint32_t d_block_size = state.world.decision_size() / 32;
		uint32_t block_index = 0;
		auto d_block_end = state.world.decision_size();
		//uint32_t block_index = (state.current_date.value & 31);
		//auto d_block_end = block_index == 31 ? state.world.decision_size() : d_block_size * (block_index + 1);
		concurrency::parallel_for(d_block_size * block_index, d_block_end, [&](uint32_t i) {
			auto d = dcon::decision_id{ dcon::decision_id::value_base_t(i) };
			auto e = state.world.decision_get_effect(d);
			if(e) {
				auto potential = state.world.decision_get_potential(d);
				auto allow = state.world.decision_get_allow(d);
				auto ai_will_do = state.world.decision_get_ai_will_do(d);
				ve::execute_serial_fast<dcon::nation_id>(state.world.nation_size(), [&](auto ids) {
					// AI-only, not dead nations
					ve::mask_vector filter_a = !state.world.nation_get_is_player_controlled(ids)
						&& state.world.nation_get_owned_province_count(ids) != 0;
					if(ve::compress_mask(filter_a).v != 0) {
						// empty allow assumed to be an "always = yes"
						ve::mask_vector filter_b = potential
							? filter_a && (trigger::evaluate(state, potential, trigger::to_generic(ids), trigger::to_generic(ids), 0))
							: filter_a;
						if(ve::compress_mask(filter_b).v != 0) {
							ve::mask_vector filter_c = allow
								? filter_b && (trigger::evaluate(state, allow, trigger::to_generic(ids), trigger::to_generic(ids), 0))
								: filter_b;
							if(ve::compress_mask(filter_c).v != 0) {
								ve::mask_vector filter_d = ai_will_do
									? filter_c && (trigger::evaluate_multiplicative_modifier(state, ai_will_do, trigger::to_generic(ids), trigger::to_generic(ids), 0) > 0.0f)
									: filter_c;
								ve::apply([&](dcon::nation_id n, bool passed_filter) {
									if(passed_filter) {
										decisions_taken.local().push_back(decision_nation_pair(d, n));
									}
								}, ids, filter_d);
							}
						}
					}
				});
			}
		});
		// combination and final execution
		auto total_vector = decisions_taken.combine([](auto& a, auto& b) {
			std::vector<decision_nation_pair, dcon::cache_aligned_allocator<decision_nation_pair>> result(a.begin(), a.end());
			result.insert(result.end(), b.begin(), b.end());
			return result;
		});
		// ensure total deterministic ordering
		pdqsort(total_vector.begin(), total_vector.end(), [&](auto a, auto b) {
			auto na = a.second;
			auto nb = b.second;
			if(na != nb)
				return na.index() < nb.index();
			return a.first.index() < b.first.index();
		});
		// assumption 1: no duplicate pair of <n, d>
		for(const auto& v : total_vector) {
			auto n = v.second;
			auto d = v.first;
			auto e = state.world.decision_get_effect(d);
			effect::execute(state, e, trigger::to_generic(n), trigger::to_generic(n), 0, uint32_t(state.current_date.value), uint32_t(n.index() << 4 ^ d.index()));
			notification::post(state, notification::message{
				[e, n, d, when = state.current_date](sys::state& state, text::layout_base& contents) {
					text::add_line(state, contents, "msg_decision_1", text::variable_type::x, n, text::variable_type::y, state.world.decision_get_name(d));
					text::add_line(state, contents, "msg_decision_2");
					ui::effect_description(state, contents, e, trigger::to_generic(n), trigger::to_generic(n), 0, uint32_t(when.value), uint32_t(n.index() << 4 ^ d.index()));
				},
				"msg_decision_title",
				n, dcon::nation_id{}, dcon::nation_id{},
				sys::message_base_type::decision
			});
		}
	}

	float estimate_pop_party_support(sys::state& state, dcon::nation_id n, dcon::political_party_id pid) {
		auto iid = state.world.political_party_get_ideology(pid);
		return state.world.nation_get_demographics(n, demographics::to_key(state, iid));
	}

	bool can_appoint_political_party(sys::state& state, dcon::nation_id n) {
		if(!politics::can_appoint_ruling_party(state, n))
			return false;
		auto last_change = state.world.nation_get_ruling_party_last_appointed(n);
		if(last_change && state.current_date < last_change + 365)
			return false;
		if(politics::is_election_ongoing(state, n))
			return false;
		return true;
	}

	bool political_party_is_state_economy(sys::state& state, dcon::political_party_id pid) {
		uint32_t combined = 0;
		for(const auto iid : state.world.in_issue) {
			combined |= state.world.political_party_get_party_issues(pid, iid).get_rules();
		}
		return ((combined & issue_rule::build_factory) != 0)
			&& ((combined & issue_rule::expand_factory) != 0);
	}

	void update_ai_ruling_party(sys::state& state) {
		for(auto n : state.world.in_nation) {
			// skip over: non ais, dead nations
			if(n.get_is_player_controlled() || n.get_owned_province_count() == 0)
				continue;

			if(ai::can_appoint_political_party(state, n)) {
				auto gov = n.get_government_type();
				auto identity = n.get_identity_from_identity_holder();
				auto start = state.world.national_identity_get_political_party_first(identity).id.index();
				auto end = start + state.world.national_identity_get_political_party_count(identity);

				dcon::political_party_id target{};
				float max_support = 0.f; //estimate_pop_party_support(state, n, state.world.nation_get_ruling_party(n));
				for(int32_t i = start; i < end; i++) {
					auto pid = dcon::political_party_id(uint16_t(i));
					if(pid != state.world.nation_get_ruling_party(n)
					&& politics::political_party_is_active(state, n, pid)
					&& (gov.get_ideologies_allowed() & ::culture::to_bits(state.world.political_party_get_ideology(pid))) != 0) {
						auto support = estimate_pop_party_support(state, n, pid);

						if(state.world.nation_get_industrial_score(n) < 15) {
							/* Speedrun being able to build factories (get the economy jumpstarted) */
							if(support > max_support
							&& ai::political_party_is_state_economy(state, pid)) {
								target = pid;
								max_support = support;
							}
						} else if(!gov.get_has_elections()) {
							/* Select what the people want -- ONLY IF WE ARE NOT DEMOCRATIC */
							if(support > max_support) {
								target = pid;
								max_support = support;
							}
						}
					}
				}

				assert(target != state.world.nation_get_ruling_party(n));
				if(target) {
					assert(command::can_appoint_ruling_party(state, n, target));
					politics::appoint_ruling_party(state, n, target);
				}
			}
		}
	}

	bool is_factory_type_active(sys::state& state, dcon::nation_id n, dcon::factory_type_id ft) {
		return (!state.world.nation_get_active_building(n, ft)
			&& !state.world.factory_type_get_is_available_from_start(ft));
	}

	bool get_is_desirable_factory_type(sys::state& state, dcon::nation_id n, dcon::factory_type_id ft) {
		if(!state.world.nation_get_active_building(n, ft)
		&& !state.world.factory_type_get_is_available_from_start(ft))
			return false;

		// does the nation need the outputs of this factory?
		if(state.world.nation_get_demand_satisfaction(n, state.world.factory_type_get_output(ft)) < 0.95f) {
			auto& inputs = state.world.factory_type_get_inputs(ft);
			for(uint32_t i = 0; i < economy::commodity_set::set_size; ++i) {
				if(inputs.commodity_type[i]) {
					// lacks input, do not build (early break)
					if(state.world.nation_get_demand_satisfaction(n, inputs.commodity_type[i]) < 0.95f) {
						return false;
					}
					continue; // -- it does not lack an input
				} else { // -- end of inputs
					return true; // iff output not satisfied, build
				}
			}
			return true; // no inputs
		}
		return false;
	}

	void get_desired_factory_types(sys::state& state, dcon::nation_id nid, std::vector<dcon::factory_type_id>& desired_types) {
		assert(desired_types.empty());
		if(desired_types.empty()) {
			for(auto type : state.world.in_factory_type) {
				if(get_is_desirable_factory_type(state, nid, type)) {
					desired_types.push_back(type.id);
				}
			}
		}
	}

	bool has_province_construction(sys::state& state, dcon::state_instance_id si, dcon::factory_type_id ft) {
		for(auto p : state.world.state_instance_get_state_building_construction(si)) {
			if(p.get_type() == ft)
				return true;
		}
		return false;
	}

	/*	Obtain a list of economy-relevant states, do not use for military stuff! This basically calculates
		the best states to invest in, w.r.t to the industrial points gained.
		By game rules this means that the most populated states are the most valuable ones. */
	void get_ordered_economy_states(sys::state& state, dcon::nation_id n, std::vector<dcon::state_instance_id, dcon::cache_aligned_allocator<dcon::state_instance_id>>& ordered_states) {
		ordered_states.clear();
		for(auto si : state.world.nation_get_state_ownership(n)) {
			if(si.get_state().get_capital().get_is_colonial() == false) {
				ordered_states.push_back(si.get_state().id);
			}
		}
		pdqsort(ordered_states.begin(), ordered_states.end(), [&](auto a, auto b) {
			auto apop = state.world.state_instance_get_demographics(a, demographics::total);
			auto bpop = state.world.state_instance_get_demographics(b, demographics::total);
			if(apop != bpop)
				return apop > bpop;
			return a.index() < b.index();
		});
	}

	void update_ai_econ_construction(sys::state& state) {
		for(auto n : state.world.in_nation) {
			// skip over: non ais, dead nations, and nations that aren't making money
			if(n.get_owned_province_count() == 0 || !n.get_is_civilized())
				continue;
			if(n.get_is_player_controlled())
				continue; // to handle the logic of player building automation later
			if(uint16_t(0.25f * n.get_recruitable_regiments()) > n.get_active_regiments())
				continue; //if our army is too small, ignore buildings:

			int32_t max_projects = std::max(5, int32_t(n.get_owned_state_count()));
			auto const rules = n.get_combined_issue_rules();
			// prepare a list of states
			static std::vector<dcon::state_instance_id, dcon::cache_aligned_allocator<dcon::state_instance_id>> ordered_states;
			ai::get_ordered_economy_states(state, n, ordered_states);
			// subsidize key industries
			if((rules & issue_rule::can_subsidise) != 0) {
				for(auto si : ordered_states) {
					auto const employable = state.world.state_instance_get_demographics(si, demographics::employable);
					auto const employed = state.world.state_instance_get_demographics(si, demographics::employed);
					auto const has_unemployed = (employable - employed) > 0.f;
					province::for_each_province_in_state_instance(state, si, [&](dcon::province_id p) {
						for(auto f : state.world.province_get_factory_location(p)) {
							// subsidize factories that are not satisfying demand
							auto const c = f.get_factory().get_building_type().get_output();
							auto is_low_prio = false;
							auto is_high_prio = false;
							if(has_unemployed
							|| state.world.nation_get_demand_satisfaction(n, c) < 0.95f
							|| c.get_total_real_demand() * 1.25f > c.get_total_production()) {
								f.get_factory().set_subsidized(true);
								is_low_prio = (c.get_total_real_demand() * 0.5f > c.get_total_production());
								is_high_prio = (c.get_total_real_demand() * 0.25f > c.get_total_production());
							}
							if((rules & issue_rule::factory_priority) != 0) {
								f.get_factory().set_priority_low(is_low_prio);
								f.get_factory().set_priority_high(is_high_prio);
							}
						}
					});
				}
			}

			if((rules & issue_rule::build_factory) != 0
			&& max_projects > 0) { // -- i.e. if building is possible
				// limit to building only if there is less than these
				for(auto si : ordered_states) {
					if(max_projects <= 0)
						break;
					/* Don't build another construction while one is ongoing */
					auto const sbc = state.world.state_instance_get_state_building_construction(si);
					if(sbc.begin() != sbc.end())
						continue;

					dcon::factory_type_id top_desired_type{};
					float top_desired_value = 0.f;
					for(const auto ft : state.world.in_factory_type) {
						if(state.world.factory_type_get_is_coastal(ft) && !province::state_is_coastal(state, si))
							continue;
						if(economy_factory::state_contains_factory(state, si, ft))
							continue;
						if(ai::get_is_desirable_factory_type(state, n, ft)) {
							auto const t_bonus = economy_factory::sum_of_factory_triggered_modifiers(state, ft, si)
								+ economy_factory::sum_of_factory_triggered_input_modifiers(state, ft, si);
							auto const sat = state.world.nation_get_demand_satisfaction(n, ft.get_output());
							auto const value = sat * (1.f + t_bonus);
							if(value > top_desired_value) {
								top_desired_type = ft;
								top_desired_value = value;
							}
						}
					}
					if(!top_desired_type)
						continue;

					// check -- either unemployed factory workers or no factory workers
					auto pw_num = state.world.state_instance_get_demographics(si, demographics::to_key(state, state.culture_definitions.primary_factory_worker));
					pw_num += state.world.state_instance_get_demographics(si, demographics::to_key(state, state.culture_definitions.secondary_factory_worker));
					auto pw_employed = state.world.state_instance_get_demographics(si, demographics::to_employment_key(state, state.culture_definitions.primary_factory_worker));
					pw_employed += state.world.state_instance_get_demographics(si, demographics::to_employment_key(state, state.culture_definitions.secondary_factory_worker));
					if(pw_employed >= float(pw_num) * 2.5f && pw_num > 0.0f)
						continue; // no spare workers

					if(state.world.factory_type_get_is_coastal(top_desired_type)
					&& !province::state_is_coastal(state, si))
						continue;

					// check: if present, try to upgrade
					bool is_present = economy_factory::state_contains_factory(state, si, top_desired_type);
					// else -- try to build -- must have room
					if(!is_present
					&& economy_factory::state_factory_count(state, si) < int32_t(state.defines.factories_per_state)
					&& max_projects > 0) {
						auto new_up = fatten(state.world, state.world.force_create_state_building_construction(si, n));
						new_up.set_remaining_construction_time(state.world.factory_type_get_construction_time(top_desired_type));
						new_up.set_is_pop_project(false);
						new_up.set_is_upgrade(false);
						new_up.set_type(top_desired_type);
						--max_projects;
					}
				} // END for(auto si : ordered_states) {
			} // END if((rules & issue_rule::build_factory) == 0)

			if((rules & issue_rule::expand_factory) != 0
			&& max_projects > 0) { // can upgrade
				for(auto si : ordered_states) {
					if(max_projects <= 0)
						break;
					/* Don't build another construction while one is ongoing */
					auto const sbc = state.world.state_instance_get_state_building_construction(si);
					if(sbc.begin() != sbc.end())
						continue;

					auto const employable = state.world.state_instance_get_demographics(si, demographics::employable);
					auto const employed = state.world.state_instance_get_demographics(si, demographics::employed);
					province::for_each_province_in_state_instance(state, si, [&](dcon::province_id p) {
						for(auto f : state.world.province_get_factory_location(p)) {
							if(max_projects <= 0)
								break;
							auto const ft = f.get_factory().get_building_type();
							if(f.get_factory().get_primary_employment() >= 0.75f
							&& f.get_factory().get_level() <= ft.get_max_level()
							&& ai::is_factory_type_active(state, n, ft)) {
								// test if factory is already upgrading
								auto ug_in_progress = false;
								for(auto c : state.world.state_instance_get_state_building_construction(si)) {
									if(c.get_type() == ft) {
										ug_in_progress = true;
										break;
									}
								}
								if(!ug_in_progress) {
									auto new_up = fatten(state.world, state.world.force_create_state_building_construction(si, n));
									new_up.set_remaining_construction_time(ft.get_construction_time());
									new_up.set_is_pop_project(false);
									new_up.set_is_upgrade(true);
									new_up.set_type(ft);
									--max_projects;
								}
							}
						}
					});
				}
			}

			static std::vector<dcon::province_id> project_provs;
			project_provs.clear();

			// try railroads
			const struct {
				bool buildable;
				dcon::province_building_type_id type;
				dcon::provincial_modifier_value mod;
			} econ_buildable[3] = {
				{
					(rules & issue_rule::build_railway) != 0,
					state.economy_definitions.railroad_building,
					sys::provincial_mod_offsets::min_build_railroad
				},
				{
					(rules & issue_rule::build_bank) != 0 && state.economy_definitions.bank_building,
					state.economy_definitions.bank_building,
					sys::provincial_mod_offsets::min_build_bank
				},
				{
					(rules & issue_rule::build_university) != 0 && state.economy_definitions.university_building,
					state.economy_definitions.university_building,
					sys::provincial_mod_offsets::min_build_university
				}
			};
			for(uint32_t i = 0; i < std::extent_v<decltype(econ_buildable)>; ++i) {
				if(econ_buildable[i].buildable && max_projects > 0) {
					project_provs.clear();
					for(auto o : n.get_province_ownership()) {
						if(n != o.get_province().get_nation_from_province_control())
							continue;
						if(military::province_is_under_siege(state, o.get_province()))
							continue;
						int32_t current_lvl = state.world.province_get_building_level(o.get_province(), econ_buildable[i].type);
						int32_t max_local_lvl = state.world.nation_get_max_building_level(n, econ_buildable[i].type);
						int32_t min_build = int32_t(state.world.province_get_modifier_values(o.get_province(), econ_buildable[i].mod));
						if(max_local_lvl - current_lvl - min_build <= 0)
							continue;
						if(!province::has_province_building_being_built(state, o.get_province(), econ_buildable[i].type)) {
							project_provs.push_back(o.get_province().id);
						}
					}
					auto cap = n.get_capital();
					pdqsort(project_provs.begin(), project_provs.end(), [&](dcon::province_id a, dcon::province_id b) {
						auto a_dist = province::sorting_distance(state, a, cap);
						auto b_dist = province::sorting_distance(state, b, cap);
						if(a_dist != b_dist)
							return a_dist < b_dist;
						return a.index() < b.index();
					});
					for(uint32_t j = 0; j < project_provs.size() && max_projects > 0; ++j) {
						auto new_proj = fatten(state.world, state.world.force_create_province_building_construction(project_provs[j], n));
						new_proj.set_remaining_construction_time(state.world.province_building_type_get_time(econ_buildable[i].type));
						new_proj.set_is_pop_project(false);
						new_proj.set_type(econ_buildable[i].type);
						--max_projects;
					}
				}
			}

			// try naval bases
			if(max_projects > 0) {
				project_provs.clear();
				for(auto o : n.get_province_ownership()) {
					if(province::can_build_naval_base(state, o.get_province(), n)) {
						project_provs.push_back(o.get_province().id);
					}
				}
				pdqsort(project_provs.begin(), project_provs.end(), [&](dcon::province_id a, dcon::province_id b) {
					auto a_weight = ai::province_strategic_weight<float>(state, a);
					auto b_weight = ai::province_strategic_weight<float>(state, b);
					if(a_weight != b_weight)
						return a_weight < b_weight;
					return a.index() < b.index();
				});
				if(!project_provs.empty()) {
					auto si = state.world.province_get_state_membership(project_provs[0]);
					if(si) {
						si.set_naval_base_is_taken(true);
					}
					auto new_rr = fatten(state.world, state.world.force_create_province_building_construction(project_provs[0], n));
					new_rr.set_remaining_construction_time(state.world.province_building_type_get_time(state.economy_definitions.naval_base_building));
					new_rr.set_is_pop_project(false);
					new_rr.set_type(state.economy_definitions.naval_base_building);
					--max_projects;
				}
			}

			/* Try buildings forts, we will dissuade the AI from building them in colonial areas or areas
			   under siege -- for obvious reasons. We will sort the best strategic provinces so we place
			   for example, forts on Alsace-Lorraine, rather than on Toulouse. Building forts on strategic
			   provinces is way more valuable than just spamming them mindlessly! */
			if(max_projects > 0) {
				project_provs.clear();
				for(auto o : n.get_province_ownership()) {
					if(n != o.get_province().get_nation_from_province_control())
						continue;
					// Here's logic to dissuade AI from building forts on useless places
					if(military::province_is_under_siege(state, o.get_province()))
						continue;
					if(o.get_province().get_is_colonial())
						continue;
					if(province::has_fort_being_built(state, o.get_province())) {
						--max_projects;
					}
					if(province::can_build_fort(state, o.get_province(), n)) {
						project_provs.push_back(o.get_province().id);
					}
				}
				pdqsort(project_provs.begin(), project_provs.end(), [&](dcon::province_id a, dcon::province_id b) {
					auto a_weight = ai::province_strategic_weight<float>(state, a);
					auto b_weight = ai::province_strategic_weight<float>(state, b);
					if(a_weight != b_weight)
						return a_weight < b_weight;
					return a.index() < b.index();
				});
				for(uint32_t i = 0; i < project_provs.size() && max_projects > 0; ++i) {
					auto new_rr = fatten(state.world, state.world.force_create_province_building_construction(project_provs[i], n));
					new_rr.set_remaining_construction_time(state.world.province_building_type_get_time(state.economy_definitions.fort_building));
					new_rr.set_is_pop_project(false);
					new_rr.set_type(state.economy_definitions.fort_building);
					--max_projects;
				}
			}
		}
	}

	void update_ai_colonial_investment(sys::state& state) {
		static std::vector<dcon::state_definition_id> investments;
		investments.clear();
		investments.resize(uint32_t(state.defines.colonial_rank));

		static std::vector<int32_t> free_points;
		free_points.clear();
		free_points.resize(uint32_t(state.defines.colonial_rank), -1);

		for(auto col : state.world.in_colonization) {
			auto n = col.get_colonizer();
			if(n.get_is_player_controlled() == false
			&& n.get_rank() <= uint16_t(state.defines.colonial_rank)
			&& !investments[n.get_rank() - 1]
			&& col.get_state().get_colonization_stage() <= uint8_t(2)
			&& state.crisis_colony != col.get_state()
			&& (!state.crisis_war || n.get_is_at_war() == false)) {
				auto crange = col.get_state().get_colonization();
				if(crange.end() - crange.begin() > 1) {
					if(col.get_last_investment() + int32_t(state.defines.colonization_days_between_investment) <= state.current_date) {
						if(free_points[n.get_rank() - 1] < 0) {
							free_points[n.get_rank() - 1] = nations::free_colonial_points(state, n);
						}
						int32_t cost = 0;;
						if(col.get_state().get_colonization_stage() == 1) {
							cost = int32_t(state.defines.colonization_interest_cost);
						} else if(col.get_level() <= 4) {
							cost = int32_t(state.defines.colonization_influence_cost);
						} else {
							cost = int32_t(state.defines.colonization_extra_guard_cost * (col.get_level() - 4) + state.defines.colonization_influence_cost);
						}
						if(free_points[n.get_rank() - 1] >= cost) {
							investments[n.get_rank() - 1] = col.get_state().id;
						}
					}
				}
			}
		}
		for(uint32_t i = 0; i < investments.size(); ++i) {
			if(investments[i]) {
				province::increase_colonial_investment(state, state.nations_by_rank[i], investments[i]);
			}
		}
	}

	void update_ai_colony_starting(sys::state& state) {
		static std::vector<int32_t> free_points;
		free_points.clear();
		free_points.resize(uint32_t(state.defines.colonial_rank), -1);
		for(int32_t i = 0; i < int32_t(state.defines.colonial_rank); ++i) {
			if(state.world.nation_get_is_player_controlled(state.nations_by_rank[i])) {
				free_points[i] = 0;
			} else {
				if(military::get_role(state, state.crisis_war, state.nations_by_rank[i]) != military::war_role::none) {
					free_points[i] = 0;
				} else {
					free_points[i] = nations::free_colonial_points(state, state.nations_by_rank[i]);
				}
			}
		}
		for(auto sd : state.world.in_state_definition) {
			if(sd.get_colonization_stage() <= 1) {
				bool has_unowned_land = false;

				dcon::province_id coastal_target;
				for(auto p : state.world.state_definition_get_abstract_state_membership(sd)) {
					if(!p.get_province().get_nation_from_province_ownership()) {
						if(p.get_province().get_is_coast() && !coastal_target) {
							coastal_target = p.get_province();
						}
						if(p.get_province().id.index() < state.province_definitions.first_sea_province.index())
							has_unowned_land = true;
					}
				}
				if(has_unowned_land) {
					for(int32_t i = 0; i < int32_t(state.defines.colonial_rank); ++i) {
						if(free_points[i] > 0) {
							bool adjacent = false;
							if(province::fast_can_start_colony(state, state.nations_by_rank[i], sd, free_points[i], coastal_target, adjacent)) {
								free_points[i] -= int32_t(state.defines.colonization_interest_cost_initial + (adjacent ? state.defines.colonization_interest_cost_neighbor_modifier : 0.0f));

								auto new_rel = fatten(state.world, state.world.force_create_colonization(sd, state.nations_by_rank[i]));
								new_rel.set_level(uint8_t(1));
								new_rel.set_last_investment(state.current_date);
								new_rel.set_points_invested(uint16_t(state.defines.colonization_interest_cost_initial + (adjacent ? state.defines.colonization_interest_cost_neighbor_modifier : 0.0f)));

								state.world.state_definition_set_colonization_stage(sd, uint8_t(1));
							}
						}
					}
				}
			}
		}
	}

	void upgrade_colonies(sys::state& state) {
		for(auto si : state.world.in_state_instance) {
			if(si.get_capital().get_is_colonial() && si.get_nation_from_state_ownership().get_is_player_controlled() == false) {
				if(province::can_integrate_colony(state, si)) {
					province::upgrade_colonial_state(state, si.get_nation_from_state_ownership(), si);
				}
			}
		}
	}

	void civilize(sys::state& state) {
		for(auto n : state.world.in_nation) {
			if(!n.get_is_player_controlled() && !n.get_is_civilized() && n.get_modifier_values(sys::national_mod_offsets::civilization_progress_modifier) >= 1.0f) {
				nations::make_civilized(state, n);
			}
		}
	}

	void take_reforms(sys::state& state) {
		for(auto n : state.world.in_nation) {
			if(n.get_is_player_controlled() || n.get_owned_province_count() == 0)
				continue;

			if(n.get_is_civilized()) { // political & social
				// Enact social policies to deter Jacobin rebels from overruning the country
				// Reactionaries will popup in effect but they are MORE weak that Jacobins
				dcon::issue_option_id iss;
				float max_support = 0.0f;

				for(auto m : state.world.nation_get_movement_within(n)) {
					if(m.get_movement().get_associated_issue_option() && m.get_movement().get_pop_support() > max_support) {
						iss = m.get_movement().get_associated_issue_option();
						max_support = m.get_movement().get_pop_support();
					}
				}
				if(!iss || !command::can_enact_issue(state, n, iss)) {
					max_support = 0.0f;
					iss = dcon::issue_option_id{};
					state.world.for_each_issue_option([&](dcon::issue_option_id io) {
						if(command::can_enact_issue(state, n, io)) {
							auto support = 0.f;
							for(const auto poid : state.world.nation_get_province_ownership_as_nation(n)) {
								for(auto plid : state.world.province_get_pop_location_as_province(poid.get_province())) {
									auto const weigth = plid.get_pop().get_size();
									support += state.world.pop_get_demographics(plid.get_pop(), pop_demographics::to_key(state, io)) * weigth;
								}
							}
							if(support > max_support) {
								iss = io;
								max_support = support;
							}
						}
					});
				}
				if(iss) {
					nations::enact_issue(state, n, iss);
				}
			} else { // military and economic
				dcon::reform_option_id cheap_r;
				float cheap_cost = 0.0f;

				auto e_mul = politics::get_economic_reform_multiplier(state, n);
				auto m_mul = politics::get_military_reform_multiplier(state, n);

				for(auto r : state.world.in_reform_option) {
					bool is_military = state.world.reform_get_reform_type(state.world.reform_option_get_parent_reform(r)) == uint8_t(culture::issue_category::military);
					auto reform = state.world.reform_option_get_parent_reform(r);
					auto current = state.world.nation_get_reforms(n, reform.id).id;
					auto allow = state.world.reform_option_get_allow(r);
					if(r.id.index() > current.index() && (!state.world.reform_get_is_next_step_only(reform.id) || current.index() + 1 == r.id.index()) && (!allow || trigger::evaluate(state, allow, trigger::to_generic(n.id), trigger::to_generic(n.id), 0))) {
						float base_cost = float(state.world.reform_option_get_technology_cost(r));
						float reform_factor = is_military ? m_mul : e_mul;
						if(!cheap_r || base_cost * reform_factor < cheap_cost) {
							cheap_cost = base_cost * reform_factor;
							cheap_r = r.id;
						}
					}
				}

				if(cheap_r && cheap_cost <= n.get_research_points()) {
					nations::enact_reform(state, n, cheap_r);
				}
			}
		}
	}

	bool will_be_crisis_primary_attacker(sys::state& state, dcon::nation_id n) {
		if(state.current_crisis == sys::crisis_type::colonial) {
			auto colonizers = state.world.state_definition_get_colonization(state.crisis_colony);
			if(colonizers.end() - colonizers.begin() < 2)
				return false;

			auto defending_colonizer = (*(colonizers.begin() + 1)).get_colonizer();
			if(state.world.nation_get_in_sphere_of(defending_colonizer) == n)
				return false;
			if(state.world.nation_get_ai_rival(n) == defending_colonizer
			|| (defending_colonizer.get_in_sphere_of() && !nations::are_allied(state, n, defending_colonizer) && state.world.nation_get_ai_rival(n) == defending_colonizer.get_in_sphere_of())) {
				return true;
			} else {
				if(state.primary_crisis_defender && state.world.nation_get_ai_rival(n) == state.primary_crisis_defender)
					return true;
				return false;
			}
		} else if(state.current_crisis == sys::crisis_type::liberation) {
			auto state_owner = state.world.state_instance_get_nation_from_state_ownership(state.crisis_state);
			auto liberated = state.world.national_identity_get_nation_from_identity_holder(state.crisis_liberation_tag);

			if(state_owner == n) //don't shoot ourselves
				return false;
			if(liberated == n) //except when we are shooting someone else
				return true;
			if(state.world.nation_get_in_sphere_of(state_owner) == n || nations::are_allied(state, n, state_owner))
				return false;
			if(state.world.nation_get_ai_rival(n) == state_owner)
				return true;
			if(state.world.nation_get_in_sphere_of(state_owner) && state.world.nation_get_ai_rival(n) == state.world.nation_get_in_sphere_of(state_owner))
				return true;
			if(state.world.nation_get_in_sphere_of(liberated) == n || nations::are_allied(state, n, liberated))
				return true;
			if(state.primary_crisis_defender && state.world.nation_get_ai_rival(n) == state.primary_crisis_defender)
				return true;
			return false;
		} else {
			return false;
		}
	}

	bool will_be_crisis_primary_defender(sys::state& state, dcon::nation_id n) {
		if(state.current_crisis == sys::crisis_type::colonial) {
			auto colonizers = state.world.state_definition_get_colonization(state.crisis_colony);
			if(colonizers.end() - colonizers.begin() < 2)
				return false;

			auto attacking_colonizer = (*colonizers.begin()).get_colonizer();

			if(state.world.nation_get_in_sphere_of(attacking_colonizer) == n)
				return false;
			if(state.world.nation_get_ai_rival(n) == attacking_colonizer
			|| (attacking_colonizer.get_in_sphere_of() && !nations::are_allied(state, n, attacking_colonizer) && state.world.nation_get_ai_rival(n) == attacking_colonizer.get_in_sphere_of())
			|| state.world.nation_get_ai_rival(n) == state.primary_crisis_attacker) {
				return true;
			} else {
				if(state.primary_crisis_attacker && state.world.nation_get_ai_rival(n) == state.primary_crisis_attacker)
					return true;
				return false;
			}
		} else if(state.current_crisis == sys::crisis_type::liberation) {
			auto state_owner = state.world.state_instance_get_nation_from_state_ownership(state.crisis_state);
			auto liberated = state.world.national_identity_get_nation_from_identity_holder(state.crisis_liberation_tag);

			if(state_owner == n) //don't shoot ourselves
				return false;
			if(liberated == n) //except when we are shooting someone else
				return true;
			if(state.world.nation_get_in_sphere_of(liberated) == n || nations::are_allied(state, n, liberated))
				return false;
			if(state.world.nation_get_ai_rival(n) == liberated)
				return true;
			if(state.world.nation_get_in_sphere_of(liberated) && state.world.nation_get_ai_rival(n) == state.world.nation_get_in_sphere_of(liberated))
				return true;
			if(state.world.nation_get_in_sphere_of(state_owner) == n || nations::are_allied(state, n, state_owner))
				return true;
			if(state.primary_crisis_attacker && state.world.nation_get_ai_rival(n) == state.primary_crisis_attacker)
				return true;
			return false;
		} else {
			return false;
		}
	}

	struct crisis_str {
		float attacker = 0.0f;
		float defender = 0.0f;
	};

	crisis_str estimate_crisis_str(sys::state& state) {
		float atotal = 0.0f;
		float dtotal = 0.0f;

		dcon::nation_id secondary_attacker;
		dcon::nation_id secondary_defender;

		if(state.current_crisis == sys::crisis_type::colonial) {
			auto colonizers = state.world.state_definition_get_colonization(state.crisis_colony);
			if(colonizers.end() - colonizers.begin() >= 2) {
				secondary_defender = (*(colonizers.begin() + 1)).get_colonizer();
				secondary_attacker = (*(colonizers.begin())).get_colonizer();
			}
		} else if(state.current_crisis == sys::crisis_type::liberation) {
			secondary_defender = state.world.state_instance_get_nation_from_state_ownership(state.crisis_state);
			secondary_attacker = state.world.national_identity_get_nation_from_identity_holder(state.crisis_liberation_tag);
		}

		if(secondary_attacker && secondary_attacker != state.primary_crisis_attacker) {
			atotal += estimate_strength(state, secondary_attacker);
		}
		if(secondary_defender && secondary_defender != state.primary_crisis_defender) {
			dtotal += estimate_strength(state, secondary_defender);
		}
		for(auto& i : state.crisis_participants) {
			if(!i.id)
				break;
			if(!i.merely_interested) {
				if(i.supports_attacker) {
					atotal += estimate_strength(state, i.id);
				} else {
					dtotal += estimate_strength(state, i.id);
				}
			}
		}
		return crisis_str{ atotal, dtotal };
	}

	bool will_join_crisis_with_offer(sys::state& state, dcon::nation_id n, sys::crisis_join_offer const& offer) {
		if(offer.target == state.world.nation_get_ai_rival(n))
			return true;
		auto offer_bits = state.world.cb_type_get_type_bits(offer.wargoal_type);
		if((offer_bits & (military::cb_flag::po_demand_state | military::cb_flag::po_annex)) != 0)
			return true;
		return false;
	}

	bool ai_offer_cb(sys::state& state, dcon::cb_type_id t) {
		auto offer_bits = state.world.cb_type_get_type_bits(t);
		if((offer_bits & (military::cb_flag::po_demand_state | military::cb_flag::po_annex)) != 0)
			return false;
		if((offer_bits & military::cb_flag::all_allowed_states) != 0)
			return false;
		if(military::cb_requires_selection_of_a_liberatable_tag(state, t))
			return false;
		if(military::cb_requires_selection_of_a_valid_nation(state, t))
			return false;
		return true;
	}

	void state_target_list(std::vector<dcon::state_instance_id>& result, sys::state& state, dcon::nation_id for_nation, dcon::nation_id within) {
		result.clear();
		for(auto const si : state.world.nation_get_state_ownership(within)) {
			result.push_back(si.get_state().id);
		}

		auto const distance_from = state.world.nation_get_capital(for_nation).id;
		int32_t first = 0;
		if(state.world.get_nation_adjacency_by_nation_adjacency_pair(for_nation, within)) {
			int32_t last = int32_t(result.size());
			while(first < last - 1) {
				while(first < last && province::state_borders_nation(state, for_nation, result[first])) {
					++first;
				}
				while(first < last - 1 && !province::state_borders_nation(state, for_nation, result[last - 1])) {
					--last;
				}
				if(first < last - 1) {
					std::swap(result[first], result[last - 1]);
					++first;
					--last;
				}
			}
		}
		if(state.world.nation_get_total_ports(for_nation) > 0 && state.world.nation_get_total_ports(within) > 0) {
			int32_t last = int32_t(result.size());
			while(first < last - 1) {
				while(first < last && province::state_is_coastal(state, result[first])) {
					++first;
				}
				while(first < last - 1 && !province::state_is_coastal(state, result[last - 1])) {
					--last;
				}
				if(first < last - 1) {
					std::swap(result[first], result[last - 1]);
					++first;
					--last;
				}
			}
		}
		pdqsort(result.begin() + first, result.end(), [&](dcon::state_instance_id a, dcon::state_instance_id b) {
			auto a_distance = province::sorting_distance(state, state.world.state_instance_get_capital(a), distance_from);
			auto b_distance = province::sorting_distance(state, state.world.state_instance_get_capital(b), distance_from);
			if(a_distance != b_distance)
				return a_distance < b_distance;
			return a.index() < b.index();
		});
	}

	void update_crisis_leaders(sys::state& state) {
		/* Either leader must be with "free" hands to be able to send peace offers */
		if(state.world.nation_get_pending_peace_offer(state.primary_crisis_attacker)
		|| state.world.nation_get_pending_peace_offer(state.primary_crisis_defender))
			return;

		if(state.crisis_temperature > 0.75f) { // make peace offer
			auto str_est = estimate_crisis_str(state);

			bool defender_victory = str_est.attacker < str_est.defender * 0.66f;
			if(state.world.nation_get_is_player_controlled(state.primary_crisis_attacker) == false) {
				assert(command::can_start_crisis_peace_offer(state, state.primary_crisis_attacker, defender_victory));
				command::execute_start_crisis_peace_offer(state, state.primary_crisis_attacker, defender_victory);
				auto pending = state.world.nation_get_peace_offer_from_pending_peace_offer(state.primary_crisis_attacker);
				for(auto& par : state.crisis_participants) {
					if(!par.id)
						break;
					bool side_ood = defender_victory ? !par.supports_attacker : par.supports_attacker;
					if(!par.merely_interested && side_ood && par.joined_with_offer.wargoal_type) {
						auto wg = fatten(state.world, state.world.create_wargoal());
						wg.set_peace_offer_from_peace_offer_item(pending);
						wg.set_added_by(par.id);
						wg.set_associated_state(par.joined_with_offer.wargoal_state);
						wg.set_associated_tag(par.joined_with_offer.wargoal_tag);
						wg.set_secondary_nation(par.joined_with_offer.wargoal_secondary_nation);
						wg.set_target_nation(par.joined_with_offer.target);
						wg.set_type(par.joined_with_offer.wargoal_type);
						assert(command::can_add_to_crisis_peace_offer(state, state.primary_crisis_attacker, par.id, par.joined_with_offer.target, par.joined_with_offer.wargoal_type, par.joined_with_offer.wargoal_state, par.joined_with_offer.wargoal_tag, par.joined_with_offer.wargoal_secondary_nation));
					}
				}
				assert(command::can_send_crisis_peace_offer(state, state.primary_crisis_attacker));
				command::execute_send_crisis_peace_offer(state, state.primary_crisis_attacker);
			} else if(state.world.nation_get_is_player_controlled(state.primary_crisis_defender) == false) {
				assert(command::can_start_crisis_peace_offer(state, state.primary_crisis_defender, !defender_victory));
				command::execute_start_crisis_peace_offer(state, state.primary_crisis_defender, !defender_victory);
				auto pending = state.world.nation_get_peace_offer_from_pending_peace_offer(state.primary_crisis_defender);
				for(auto& par : state.crisis_participants) {
					if(!par.id)
						break;
					bool side_ood = !defender_victory ? par.supports_attacker : !par.supports_attacker;
					if(!par.merely_interested && side_ood && par.joined_with_offer.wargoal_type) {
						auto wg = fatten(state.world, state.world.create_wargoal());
						wg.set_peace_offer_from_peace_offer_item(pending);
						wg.set_added_by(par.id);
						wg.set_associated_state(par.joined_with_offer.wargoal_state);
						wg.set_associated_tag(par.joined_with_offer.wargoal_tag);
						wg.set_secondary_nation(par.joined_with_offer.wargoal_secondary_nation);
						wg.set_target_nation(par.joined_with_offer.target);
						wg.set_type(par.joined_with_offer.wargoal_type);
						assert(command::can_add_to_crisis_peace_offer(state, state.primary_crisis_defender, par.id, par.joined_with_offer.target, par.joined_with_offer.wargoal_type, par.joined_with_offer.wargoal_state, par.joined_with_offer.wargoal_tag, par.joined_with_offer.wargoal_secondary_nation));
					}
				}
				assert(command::can_send_crisis_peace_offer(state, state.primary_crisis_defender));
				command::execute_send_crisis_peace_offer(state, state.primary_crisis_defender);
			}
		} else if(state.crisis_temperature > 0.2f) { // recruit nations
			auto str_est = estimate_crisis_str(state);
			if(str_est.attacker < str_est.defender && state.world.nation_get_is_player_controlled(state.primary_crisis_attacker) == false) {
				for(auto& par : state.crisis_participants) {
					if(!par.id)
						break;
					if(par.merely_interested) {
						auto other_cbs = state.world.nation_get_available_cbs(par.id);
						dcon::cb_type_id offer_cb;
						dcon::nation_id target;

						[&]() {
							for(auto& op_par : state.crisis_participants) {
								if(!op_par.id)
									break;
								if(!op_par.merely_interested && op_par.supports_attacker == false) {
									for(auto& cb : other_cbs) {
										if(cb.target == op_par.id && ai_offer_cb(state, cb.cb_type) && military::cb_conditions_satisfied(state, par.id, op_par.id, cb.cb_type)) {
											offer_cb = cb.cb_type;
											target = op_par.id;
											return;
										}
									}
									for(auto cb : state.world.in_cb_type) {
										if((cb.get_type_bits() & military::cb_flag::always) != 0) {
											if(ai_offer_cb(state, cb) && military::cb_conditions_satisfied(state, par.id, op_par.id, cb)) {
												offer_cb = cb;
												target = op_par.id;
												return;
											}
										}
									}
								}
							}
							}();

							if(offer_cb) {
								if(military::cb_requires_selection_of_a_state(state, offer_cb)) {
									std::vector < dcon::state_instance_id> potential_states;
									state_target_list(potential_states, state, par.id, target);
									for(auto s : potential_states) {
										if(military::cb_instance_conditions_satisfied(state, par.id, target, offer_cb, state.world.state_instance_get_definition(s), dcon::national_identity_id{}, dcon::nation_id{})) {
											diplomatic_message::message m{};
											m.to = par.id;
											m.from = state.primary_crisis_attacker;
											m.data.crisis_offer.target = target;
											m.data.crisis_offer.wargoal_secondary_nation = dcon::nation_id{};
											m.data.crisis_offer.wargoal_state = state.world.state_instance_get_definition(s);
											m.data.crisis_offer.wargoal_tag = dcon::national_identity_id{};
											m.data.crisis_offer.wargoal_type = offer_cb;
											m.type = diplomatic_message::type::take_crisis_side_offer;
											diplomatic_message::post(state, m);

											break;
										}
									}
								} else {
									diplomatic_message::message m{};
									m.to = par.id;
									m.from = state.primary_crisis_attacker;
									m.data.crisis_offer.target = target;
									m.data.crisis_offer.wargoal_secondary_nation = dcon::nation_id{};
									m.data.crisis_offer.wargoal_state = dcon::state_definition_id{};
									m.data.crisis_offer.wargoal_tag = dcon::national_identity_id{};
									m.data.crisis_offer.wargoal_type = offer_cb;
									m.type = diplomatic_message::type::take_crisis_side_offer;
									diplomatic_message::post(state, m);
								}
							}
					}
				}
			} else if(str_est.attacker < str_est.defender && state.world.nation_get_is_player_controlled(state.primary_crisis_defender) == false) {
				for(auto& par : state.crisis_participants) {
					if(!par.id)
						break;
					if(par.merely_interested) {
						auto other_cbs = state.world.nation_get_available_cbs(par.id);
						dcon::cb_type_id offer_cb;
						dcon::nation_id target;

						[&]() {
							for(auto& op_par : state.crisis_participants) {
								if(!op_par.id)
									break;
								if(!op_par.merely_interested && op_par.supports_attacker == true) {
									for(auto& cb : other_cbs) {
										if(cb.target == op_par.id && ai_offer_cb(state, cb.cb_type) && military::cb_conditions_satisfied(state, par.id, op_par.id, cb.cb_type)) {
											offer_cb = cb.cb_type;
											target = op_par.id;
											return;
										}
									}
									for(auto cb : state.world.in_cb_type) {
										if((cb.get_type_bits() & military::cb_flag::always) != 0) {
											if(ai_offer_cb(state, cb) && military::cb_conditions_satisfied(state, par.id, op_par.id, cb)) {
												offer_cb = cb;
												target = op_par.id;
												return;
											}
										}
									}
								}
							}
							}();

							if(offer_cb) {
								if(military::cb_requires_selection_of_a_state(state, offer_cb)) {
									std::vector < dcon::state_instance_id> potential_states;
									state_target_list(potential_states, state, par.id, target);
									for(auto s : potential_states) {
										if(military::cb_instance_conditions_satisfied(state, par.id, target, offer_cb, state.world.state_instance_get_definition(s), dcon::national_identity_id{}, dcon::nation_id{})) {
											diplomatic_message::message m{};
											m.to = par.id;
											m.from = state.primary_crisis_defender;
											m.data.crisis_offer.target = target;
											m.data.crisis_offer.wargoal_secondary_nation = dcon::nation_id{};
											m.data.crisis_offer.wargoal_state = state.world.state_instance_get_definition(s);
											m.data.crisis_offer.wargoal_tag = dcon::national_identity_id{};
											m.data.crisis_offer.wargoal_type = offer_cb;
											m.type = diplomatic_message::type::take_crisis_side_offer;
											diplomatic_message::post(state, m);
											break;
										}
									}
								} else {
									diplomatic_message::message m{};
									m.to = par.id;
									m.from = state.primary_crisis_defender;
									m.data.crisis_offer.target = target;
									m.data.crisis_offer.wargoal_secondary_nation = dcon::nation_id{};
									m.data.crisis_offer.wargoal_state = dcon::state_definition_id{};
									m.data.crisis_offer.wargoal_tag = dcon::national_identity_id{};
									m.data.crisis_offer.wargoal_type = offer_cb;
									m.type = diplomatic_message::type::take_crisis_side_offer;
									diplomatic_message::post(state, m);
								}
							}
					}
				}
			}
		}
	}

	bool will_accept_crisis_peace_offer(sys::state& state, dcon::nation_id to, bool is_concession, bool missing_wg) {
		if(state.crisis_temperature < 50.0f)
			return false;

		auto str_est = estimate_crisis_str(state);

		if(to == state.primary_crisis_attacker) {
			if(str_est.attacker < str_est.defender * 0.66f)
				return true;
			if(str_est.attacker < str_est.defender * 0.75f)
				return is_concession;

			if(!is_concession)
				return false;

			dcon::nation_id attacker = state.primary_crisis_attacker;
			if(state.current_crisis == sys::crisis_type::colonial) {
				auto colonizers = state.world.state_definition_get_colonization(state.crisis_colony);
				if(colonizers.end() - colonizers.begin() >= 2) {
					attacker = (*(colonizers.begin())).get_colonizer();
				}
			}

			if(missing_wg)
				return false;

			return true;
		} else if(to == state.primary_crisis_defender) {
			if(str_est.defender < str_est.attacker * 0.66f)
				return true;
			if(str_est.defender < str_est.attacker * 0.75f)
				return is_concession;

			if(!is_concession)
				return false;

			if(missing_wg)
				return false;

			return true;
		}
		return false;
	}

	bool will_accept_crisis_peace_offer(sys::state& state, dcon::nation_id to, dcon::peace_offer_id peace) {
		if(state.crisis_temperature < 50.0f)
			return false;

		auto str_est = estimate_crisis_str(state);

		if(to == state.primary_crisis_attacker) {
			if(str_est.attacker < str_est.defender * 0.66f)
				return true;
			if(str_est.attacker < str_est.defender * 0.75f)
				return state.world.peace_offer_get_is_concession(peace);

			if(!state.world.peace_offer_get_is_concession(peace))
				return false;

			dcon::nation_id attacker = state.primary_crisis_attacker;
			if(state.current_crisis == sys::crisis_type::colonial) {
				auto colonizers = state.world.state_definition_get_colonization(state.crisis_colony);
				if(colonizers.end() - colonizers.begin() >= 2) {
					attacker = (*(colonizers.begin())).get_colonizer();
				}
			}
			{
				bool missing_wg = true;
				for(auto wg : state.world.peace_offer_get_peace_offer_item(peace)) {
					if(wg.get_wargoal().get_added_by() == attacker)
						missing_wg = false;
				}
				if(missing_wg)
					return false;
			}

			for(auto& i : state.crisis_participants) {
				if(!i.id)
					break;
				if(!i.merely_interested) {
					if(i.supports_attacker && i.joined_with_offer.wargoal_type) {
						bool missing_wg = true;
						for(auto wg : state.world.peace_offer_get_peace_offer_item(peace)) {
							if(wg.get_wargoal().get_added_by() == i.id)
								missing_wg = false;
						}
						if(missing_wg)
							return false;
					}
				}
			}
			return true;

		} else if(to == state.primary_crisis_defender) {
			if(str_est.defender < str_est.attacker * 0.66f)
				return true;
			if(str_est.defender < str_est.attacker * 0.75f)
				return state.world.peace_offer_get_is_concession(peace);

			if(!state.world.peace_offer_get_is_concession(peace))
				return false;

			if(state.current_crisis == sys::crisis_type::colonial) {
				auto colonizers = state.world.state_definition_get_colonization(state.crisis_colony);
				if(colonizers.end() - colonizers.begin() >= 2) {
					auto defender = (*(colonizers.begin() + 1)).get_colonizer();

					bool missing_wg = true;
					for(auto wg : state.world.peace_offer_get_peace_offer_item(peace)) {
						if(wg.get_wargoal().get_added_by() == defender)
							missing_wg = false;
					}
					if(missing_wg)
						return false;
				}
			}

			for(auto& i : state.crisis_participants) {
				if(!i.id)
					break;
				if(!i.merely_interested) {
					if(!i.supports_attacker && i.joined_with_offer.wargoal_type) {
						bool missing_wg = true;
						for(auto wg : state.world.peace_offer_get_peace_offer_item(peace)) {
							if(wg.get_wargoal().get_added_by() == i.id)
								missing_wg = false;
						}
						if(missing_wg)
							return false;
					}
				}
			}
			return true;
		}
		return false;
	}

	void update_war_intervention(sys::state& state) {
		for(auto& gp : state.great_nations) {
			if(state.world.nation_get_is_player_controlled(gp.nation) == false && state.world.nation_get_is_at_war(gp.nation) == false) {
				bool as_attacker = false;
				dcon::war_id intervention_target;
				[&]() {
					for(auto w : state.world.in_war) {
						//GPs will try to intervene in wars to protect smaller nations in the same cultural union
						if(command::can_intervene_in_war(state, gp.nation, w, false)) {
							auto par = state.world.war_get_primary_defender(w);
							if(state.world.nation_get_primary_culture(gp.nation).get_group_from_culture_group_membership() == state.world.nation_get_primary_culture(par).get_group_from_culture_group_membership()
							&& !nations::is_great_power(state, par)
							) {
								intervention_target = w;
								return;
							}
						}
						if(w.get_is_great()) {
							if(command::can_intervene_in_war(state, gp.nation, w, false)) {
								for(auto par : w.get_war_participant()) {
									if(par.get_is_attacker() && military::can_use_cb_against(state, gp.nation, par.get_nation())) {
										intervention_target = w;
										return;
									}
								}
							}
							if(command::can_intervene_in_war(state, gp.nation, w, true)) {
								for(auto par : w.get_war_participant()) {
									if(!par.get_is_attacker() && military::can_use_cb_against(state, gp.nation, par.get_nation())) {
										intervention_target = w;
										as_attacker = true;
										return;
									}
								}
							}
						} else if(military::get_role(state, w, state.world.nation_get_ai_rival(gp.nation)) == military::war_role::attacker) {
							if(command::can_intervene_in_war(state, gp.nation, w, false)) {
								intervention_target = w;
								return;
							}
						}
					}
					}();
					if(intervention_target) {
						assert(command::can_intervene_in_war(state, gp.nation, intervention_target, as_attacker));
						command::execute_intervene_in_war(state, gp.nation, intervention_target, as_attacker);
					}
			}
		}
	}

	dcon::cb_type_id pick_fabrication_type(sys::state& state, dcon::nation_id from, dcon::nation_id target) {
		static std::vector<dcon::cb_type_id> possibilities;
		possibilities.clear();

		/* Wanted PO bits */
		auto po_bits = (military::cb_flag::po_demand_state | military::cb_flag::po_annex);
		if(nations::is_great_power(state, target) && nations::is_great_power(state, from)) {
			po_bits |= military::cb_flag::po_disarmament;
		}

		for(auto c : state.world.in_cb_type) {
			auto bits = state.world.cb_type_get_type_bits(c);
			if((bits & (military::cb_flag::always | military::cb_flag::is_not_constructing_cb)) != 0)
				continue;
			if((bits & po_bits) == 0)
				continue;
			// Uncivilized nations are more aggressive to westernize faster
			auto const infamy_limit = state.world.nation_get_is_civilized(from)
				? state.defines.badboy_limit * 0.5f : state.defines.badboy_limit;
			if(state.world.nation_get_infamy(from) + military::cb_infamy(state, c) >= infamy_limit)
				continue;
			if(!military::cb_conditions_satisfied(state, from, target, c))
				continue;
			auto sl = state.world.nation_get_in_sphere_of(target);
			if(sl == from)
				continue;
			possibilities.push_back(c);
		}
		if(!possibilities.empty()) {
			return possibilities[rng::reduce(uint32_t(rng::get_random(state, uint32_t((from.index() << 3) ^ target.index()))), uint32_t(possibilities.size()))];
		}
		return dcon::cb_type_id{};
	}

	/*	Tests if a nation (via game rules) can go to war with another given nation (this is not criteria for selecting the
		best nation to go war with for the AI, it's only a basic game rule check)
		@param n Nation that wants to go to war
		@param real_target The real target of the declaration (i.e the overlord)
		@param other The "involved" target of the declaration (i.e a puppet) */
	bool can_go_war_with(sys::state& state, dcon::nation_id n, dcon::nation_id real_target, dcon::nation_id other) {
		if(real_target == n || other == n)
			return false;
		if(state.world.nation_get_owned_province_count(real_target) == 0
		|| state.world.nation_get_owned_province_count(other) == 0)
			return false;
		if(state.world.nation_get_in_sphere_of(real_target) == n
		|| state.world.nation_get_in_sphere_of(other) == n)
			return false;
		/* Declaring on nations in the same sphere is a big no no */
		if(state.world.nation_get_in_sphere_of(n)
		&& state.world.nation_get_in_sphere_of(n) == state.world.nation_get_in_sphere_of(real_target))
			return false;
		if(nations::are_allied(state, n, real_target) || nations::are_allied(state, n, other))
			return false;
		if(military::are_allied_in_war(state, n, other) || military::are_allied_in_war(state, n, real_target))
			return false;
		if(military::has_truce_with(state, n, other) || military::has_truce_with(state, n, real_target))
			return false;
		for(auto const pc : state.world.nation_get_province_ownership(real_target)) {
			for(auto const al : pc.get_province().get_army_location()) {
				if(al.get_army().get_controller_from_army_control() == n) {
					return false;
				}
			}
		}
		return military::can_use_cb_against(state, n, other);
	}

	bool valid_construction_target(sys::state& state, dcon::nation_id from, dcon::nation_id target) {
		// Copied from commands.cpp:can_fabricate_cb()
		if(from == target)
			return false;
		auto ol = state.world.nation_get_overlord_as_subject(from);
		if(state.world.overlord_get_ruler(ol) && state.world.overlord_get_ruler(ol) != target)
			return false;
		if(state.world.nation_get_in_sphere_of(target) == from)
			return false;
		if(military::are_at_war(state, target, from))
			return false;
		if(military::has_truce_with(state, target, from))
			return false;
		// Not game rules, but checks for other thingies -- like declaring on our spherelord is no no
		auto sl = state.world.nation_get_in_sphere_of(target);
		if(sl && sl == state.world.nation_get_in_sphere_of(from))
			return false;
		// Attacking people from other regions only if we have naval superiority
		bool has_adj = false;
		for(const auto adj : state.world.nation_get_nation_adjacency(target)) {
			auto const other = adj.get_connected_nations(adj.get_connected_nations(0) == target ? 1 : 0);
			if(other == from) {
				has_adj = true;
				break;
			} else if(auto ovr = other.get_overlord_as_subject(); ovr && ovr.get_ruler() == from) {
				has_adj = true;
				break;
			}
		}
		if(!has_adj
		&& state.world.nation_get_capital_ship_score(from) < std::max(1.f, 1.25f * state.world.nation_get_capital_ship_score(target))) {
			return false;
		}
		//auto const ovr = state.world.nation_get_overlord_as_subject(target);
		//auto const real_target = state.world.overlord_get_ruler(ovr) ? state.world.overlord_get_ruler(ovr) : target;
		return true;
	}

	void update_cb_fabrication(sys::state& state) {
		for(auto n : state.world.in_nation) {
			if(n.get_is_player_controlled()
			|| n.get_owned_province_count() == 0
			|| n.get_is_at_war()
			|| n.get_constructing_cb_type()
			|| n.get_diplomatic_points() < state.defines.make_cb_diplomatic_cost
			|| n.get_military_score() == 0)
				continue;
			if(auto ll = n.get_last_war_loss(); ll && state.current_date <= (ll + 365 * 5))
				continue;
			auto const infamy_limit = state.world.nation_get_is_civilized(n)
				? state.defines.badboy_limit * 0.5f : state.defines.badboy_limit;
			if(n.get_infamy() >= infamy_limit)
				continue;
			/* Aggression base, in the base game its 5, so a 5% chance per each evaluation */
			auto chance = rng::get_random_float(state, uint32_t(n.id.index()));
			if(chance * 100.f >= state.defines.aggression_base)
				continue;
			/* Compile weights of most desirable nation */
			auto const base_strength = estimate_strength(state, n);
			static std::vector<dcon::nation_id> possible_targets;
			possible_targets.clear();
			for(auto i : state.world.in_nation) {
				if(ai::valid_construction_target(state, n, i)) {
					auto const weight = ai::war_weight_potential_target(state, n, i, base_strength);
					if(weight > 0.f) {
						possible_targets.push_back(i.id); 
					}
				}
			}
			if(!possible_targets.empty()) {
				auto t = possible_targets[rng::reduce(uint32_t(rng::get_random(state, uint32_t(n.id.index())) >> 2), uint32_t(possible_targets.size()))];
				if(auto cb = ai::pick_fabrication_type(state, n, t); cb) {
					n.set_constructing_cb_target(t);
					n.set_constructing_cb_type(cb);
				}
			}
		}
	}

	bool will_join_war(sys::state& state, dcon::nation_id n, dcon::war_id w, bool as_attacker) {
		/* Forbid war between uncivs against civs of other continent except if they are a sphereling or substate */
		if(!state.world.nation_get_is_civilized(n)
		&& !state.world.nation_get_is_substate(n) && !state.world.nation_get_in_sphere_of(n)
		&& !state.world.overlord_get_ruler(state.world.nation_get_overlord_as_subject(n))) {
			auto pa = state.world.war_get_primary_attacker(w);
			auto pd = state.world.war_get_primary_defender(w);
			auto pa_cap = state.world.nation_get_capital(pa);
			auto pd_cap = state.world.nation_get_capital(pd);
			auto cap = state.world.nation_get_capital(n);
			if(state.world.province_get_continent(pa_cap) != state.world.province_get_continent(cap))
				return false;
			if(state.world.province_get_continent(pd_cap) != state.world.province_get_continent(cap))
				return false;
		}
		for(auto par : state.world.war_get_war_participant(w)) {
			if(par.get_is_attacker() == !as_attacker) {
				// Could use a CB against this nation?
				if(military::can_use_cb_against(state, n, par.get_nation()))
					return true;
				// Eager to absolutely demolish our rival if possible
				if(state.world.nation_get_ai_rival(n) == par.get_nation())
					return true;
			}
		}
		return false;
	}

	struct possible_cb {
		dcon::nation_id target;
		dcon::nation_id secondary_nation;
		dcon::national_identity_id associated_tag;
		dcon::state_definition_id state_def;
		dcon::cb_type_id cb;
	};

	float possible_cb_weight(sys::state& state, dcon::nation_id n, possible_cb const& a) {
		float weight = 1.f;
		if((state.world.cb_type_get_type_bits(a.cb) & military::cb_flag::po_annex) != 0) {
			weight *= 5.f;
		}
		if((state.world.cb_type_get_type_bits(a.cb) & military::cb_flag::po_demand_state) != 0) {
			weight *= 2.5f;
		}
		/* AI will never disarm anyone who isn't their rival -- because why would they */
		if(state.world.nation_get_ai_rival(n) == a.target
		&& (state.world.cb_type_get_type_bits(a.cb) & military::cb_flag::po_disarmament) != 0) {
			weight *= 1.5f;
		}
		if(state.world.nation_get_ai_rival(n) == a.target) {
			if(a.associated_tag || a.secondary_nation) {
				/* Taking from sphere CBs are very wasteful -- and rarely ever useful */
				if((state.world.cb_type_get_type_bits(a.cb) & military::cb_flag::po_add_to_sphere) != 0) {
					weight *= 0.5f;
				}
				if((state.world.cb_type_get_type_bits(a.cb) & military::cb_flag::po_transfer_provinces) != 0) {
					weight *= 5.f;
				}
			} else {
				weight *= 5.f;
			}
		}
		return weight;
	}

	void place_instance_in_result_war(sys::state& state, std::vector<possible_cb>& result, dcon::nation_id n, dcon::nation_id target, dcon::war_id w, dcon::cb_type_id cb, std::vector<dcon::state_instance_id> const& target_states) {
		auto can_use = state.world.cb_type_get_can_use(cb);
		auto allowed_substates = state.world.cb_type_get_allowed_substate_regions(cb);
		if(allowed_substates) {
			if(!state.world.nation_get_is_substate(target))
				return;
			auto ruler = state.world.overlord_get_ruler(state.world.nation_get_overlord_as_subject(target));
			if(can_use && !trigger::evaluate(state, can_use, trigger::to_generic(ruler), trigger::to_generic(n), trigger::to_generic(ruler))) {
				return;
			}
		} else {
			if(can_use && !trigger::evaluate(state, can_use, trigger::to_generic(target), trigger::to_generic(n), trigger::to_generic(target))) {
				return;
			}
		}

		auto allowed_countries = state.world.cb_type_get_allowed_countries(cb);
		auto allowed_states = state.world.cb_type_get_allowed_states(cb);

		if(!allowed_countries && allowed_states) {
			for(auto si : target_states) {
				if(trigger::evaluate(state, allowed_states, trigger::to_generic(si), trigger::to_generic(n), trigger::to_generic(n))) {
					if(!military::war_goal_would_be_duplicate(state, n, w, target, cb, state.world.state_instance_get_definition(si), dcon::national_identity_id{}, dcon::nation_id{})) {
						assert(military::cb_conditions_satisfied(state, n, target, cb));
						result.push_back(possible_cb{ target, dcon::nation_id{}, dcon::national_identity_id{}, state.world.state_instance_get_definition(si), cb });
						return;
					}
				}
			}
			return;
		} else if(allowed_substates) { // checking for whether the target is a substate is already done above
			for(auto si : target_states) {
				if(trigger::evaluate(state, allowed_substates, trigger::to_generic(si), trigger::to_generic(n), trigger::to_generic(n))) {
					if(!military::war_goal_would_be_duplicate(state, n, w, target, cb, state.world.state_instance_get_definition(si), dcon::national_identity_id{}, dcon::nation_id{})) {
						assert(military::cb_conditions_satisfied(state, n, target, cb));
						result.push_back(possible_cb{ target, dcon::nation_id{}, dcon::national_identity_id{}, state.world.state_instance_get_definition(si), cb });
						return;
					}
				}
			}
			return;
		} else if(allowed_countries) {
			bool liberate = (state.world.cb_type_get_type_bits(cb) & military::cb_flag::po_transfer_provinces) != 0;
			for(auto other_nation : state.world.in_nation) {
				if(other_nation != n) {
					if(trigger::evaluate(state, allowed_countries, trigger::to_generic(target), trigger::to_generic(n), trigger::to_generic(other_nation.id))) {
						if(allowed_states) { // check whether any state within the target is valid for free / liberate
							for(auto i = target_states.size(); i-- > 0;) {
								auto si = target_states[i];
								if(trigger::evaluate(state, allowed_states, trigger::to_generic(si), trigger::to_generic(n), trigger::to_generic(other_nation.id))) {
									if(liberate) {
										if(!military::war_goal_would_be_duplicate(state, n, w, target, cb, state.world.state_instance_get_definition(si), other_nation.get_identity_from_identity_holder(), dcon::nation_id{})) {
											assert(military::cb_conditions_satisfied(state, n, target, cb));
											result.push_back(possible_cb{ target, dcon::nation_id{}, other_nation.get_identity_from_identity_holder(), state.world.state_instance_get_definition(si), cb });
											return;
										}
									} else {
										if(!military::war_goal_would_be_duplicate(state, n, w, target, cb, state.world.state_instance_get_definition(si), dcon::national_identity_id{}, other_nation)) {
											assert(military::cb_conditions_satisfied(state, n, target, cb));
											result.push_back(possible_cb{ target, other_nation, dcon::national_identity_id{}, state.world.state_instance_get_definition(si), cb });
											return;
										}
									}
								}
							}
						} else { // no allowed states trigger
							if(liberate) {
								if(!military::war_goal_would_be_duplicate(state, n, w, target, cb, dcon::state_definition_id{}, other_nation.get_identity_from_identity_holder(), dcon::nation_id{})) {
									assert(military::cb_conditions_satisfied(state, n, target, cb));
									result.push_back(possible_cb{ target, dcon::nation_id{}, other_nation.get_identity_from_identity_holder(), dcon::state_definition_id{}, cb });
									return;
								}
							} else {
								if(!military::war_goal_would_be_duplicate(state, n, w, target, cb, dcon::state_definition_id{}, dcon::national_identity_id{}, other_nation)) {
									assert(military::cb_conditions_satisfied(state, n, target, cb));
									result.push_back(possible_cb{ target, other_nation, dcon::national_identity_id{}, dcon::state_definition_id{}, cb });
									return;
								}
							}
						}
					}
				}
			}
			return;
		} else if(!military::war_goal_would_be_duplicate(state, n, w, target, cb, dcon::state_definition_id{}, dcon::national_identity_id{}, dcon::nation_id{})) {
			assert(military::cb_conditions_satisfied(state, n, target, cb));
			result.push_back(possible_cb{ target, dcon::nation_id{}, dcon::national_identity_id{}, dcon::state_definition_id{}, cb });
			return;
		}
	}

	bool can_add_unjustified_cbs(sys::state& state, dcon::nation_id n, bool is_gw) {
		auto const total_pop = state.world.nation_get_demographics(n, demographics::total);
		auto const total_jingo = state.world.nation_get_demographics(n, demographics::to_key(state, state.culture_definitions.jingoism));
		auto const jingoism_perc = total_pop > 0.f ? total_jingo / total_pop : 0.0f;
		auto const jingo_req_mod = (is_gw ? state.defines.gw_wargoal_jingoism_requirement_mod : 1.f);
		return jingoism_perc >= state.defines.wargoal_jingoism_requirement * jingo_req_mod;
	}

	/* Obtains a list of usable CBs
		@param add_unjustified Can use CBs that will incur no jingoism/infamy cost */
	void sort_cbs_for_war(std::vector<possible_cb>& result, sys::state& state, dcon::nation_id n, dcon::war_id w, bool add_unjustified) {
		result.clear();
		auto is_attacker = military::get_role(state, w, n) == military::war_role::attacker;
		for(auto par : state.world.war_get_war_participant(w)) {
			if(par.get_is_attacker() != is_attacker) {
				static std::vector<dcon::state_instance_id> target_states;
				target_states.clear();
				state_target_list(target_states, state, n, par.get_nation());

				/* Don't add duplicates of CBs for no reason */
				ve::vectorizable_buffer<bool, dcon::cb_type_id> cb_buf(state.world.cb_type_size());
				for(auto const& cb : state.world.nation_get_available_cbs(n)) {
					if(cb.target == par.get_nation()) {
						place_instance_in_result_war(state, result, n, par.get_nation(), w, cb.cb_type, target_states);
						cb_buf.set(cb.cb_type, true);
					}
				}
				for(auto const cb : state.world.in_cb_type) {
					if(!cb_buf.get(cb)) {
						if((cb.get_type_bits() & military::cb_flag::always) != 0) {
							if(military::cb_conditions_satisfied(state, n, par.get_nation(), cb)) {
								place_instance_in_result_war(state, result, n, par.get_nation(), w, cb, target_states);
							}
						} else if(add_unjustified
						&& (cb.get_type_bits() & military::cb_flag::is_not_constructing_cb) == 0) { /* always = 0 */
							if(military::cb_conditions_satisfied(state, n, par.get_nation(), cb)) {
								/* Don't be an idiot and go over infamy limit */
								auto const free_infamy = state.defines.badboy_limit - state.world.nation_get_infamy(n);
								if(military::cb_infamy(state, cb) < free_infamy) {
									place_instance_in_result_war(state, result, n, par.get_nation(), w, cb, target_states);
								}
							}
						}
					}
				}
			}
		}
		pdqsort(result.begin(), result.end(), [&](possible_cb const& a, possible_cb const& b) {
			/* Actual sort weighting */
			auto a_weight = ai::possible_cb_weight(state, n, a);
			auto b_weight = ai::possible_cb_weight(state, n, b);
			if(a_weight != b_weight)
				return a_weight > b_weight;
			/* Preserve total order */
			if(a.cb != b.cb)
				return a.cb.index() < b.cb.index();
			if(a.target != b.target)
				return a.target.index() < b.target.index();
			if(a.secondary_nation != b.secondary_nation)
				return a.secondary_nation.index() < b.secondary_nation.index();
			if(a.associated_tag != b.associated_tag)
				return a.associated_tag.index() < b.associated_tag.index();
			return a.state_def.index() < b.state_def.index();
		});
	}

	void place_instance_in_result(sys::state& state, std::vector<possible_cb>& result, dcon::nation_id n, dcon::nation_id target, dcon::cb_type_id cb, std::vector<dcon::state_instance_id> const& target_states) {
		auto can_use = state.world.cb_type_get_can_use(cb);
		auto allowed_substates = state.world.cb_type_get_allowed_substate_regions(cb);

		if(allowed_substates) {
			if(!state.world.nation_get_is_substate(target))
				return;
			auto ruler = state.world.overlord_get_ruler(state.world.nation_get_overlord_as_subject(target));
			if(can_use && !trigger::evaluate(state, can_use, trigger::to_generic(ruler), trigger::to_generic(n), trigger::to_generic(ruler))) {
				return;
			}
		} else {
			if(can_use && !trigger::evaluate(state, can_use, trigger::to_generic(target), trigger::to_generic(n), trigger::to_generic(target))) {
				return;
			}
		}

		auto allowed_countries = state.world.cb_type_get_allowed_countries(cb);
		auto allowed_states = state.world.cb_type_get_allowed_states(cb);

		if(!allowed_countries && allowed_states) {
			bool any_allowed = false;
			for(auto si : target_states) {
				if(trigger::evaluate(state, allowed_states, trigger::to_generic(si), trigger::to_generic(n), trigger::to_generic(n))) {
					assert(military::cb_conditions_satisfied(state, n, target, cb));
					result.push_back(possible_cb{ target, dcon::nation_id{}, dcon::national_identity_id{}, state.world.state_instance_get_definition(si), cb });
					return;
				}
			}
			return;
		} else if(allowed_substates) { // checking for whether the target is a substate is already done above
			for(auto si : target_states) {
				if(trigger::evaluate(state, allowed_substates, trigger::to_generic(si), trigger::to_generic(n), trigger::to_generic(n))) {
					assert(military::cb_conditions_satisfied(state, n, target, cb));
					result.push_back(possible_cb{ target, dcon::nation_id{}, dcon::national_identity_id{}, state.world.state_instance_get_definition(si), cb });
					return;
				}
			}
			return;
		} else if(allowed_countries) {
			bool liberate = (state.world.cb_type_get_type_bits(cb) & military::cb_flag::po_transfer_provinces) != 0;
			for(auto other_nation : state.world.in_nation) {
				if(other_nation != n) {
					if(trigger::evaluate(state, allowed_countries, trigger::to_generic(target), trigger::to_generic(n),
						trigger::to_generic(other_nation.id))) {
						if(allowed_states) { // check whether any state within the target is valid for free / liberate
							for(auto i = target_states.size(); i-- > 0;) {
								auto si = target_states[i];
								if(trigger::evaluate(state, allowed_states, trigger::to_generic(si), trigger::to_generic(n), trigger::to_generic(other_nation.id))) {
									if(liberate) {
										assert(military::cb_conditions_satisfied(state, n, target, cb));
										result.push_back(possible_cb{ target, dcon::nation_id{}, other_nation.get_identity_from_identity_holder(), state.world.state_instance_get_definition(si), cb });
										return;
									} else {
										assert(military::cb_conditions_satisfied(state, n, target, cb));
										result.push_back(possible_cb{ target, other_nation, dcon::national_identity_id{}, state.world.state_instance_get_definition(si), cb });
										return;
									}
								}
							}
						} else { // no allowed states trigger
							if(liberate) {
								assert(military::cb_conditions_satisfied(state, n, target, cb));
								result.push_back(possible_cb{ target, dcon::nation_id{}, other_nation.get_identity_from_identity_holder(), dcon::state_definition_id{}, cb });
								return;
							} else {
								assert(military::cb_conditions_satisfied(state, n, target, cb));
								result.push_back(possible_cb{ target, other_nation, dcon::national_identity_id{}, dcon::state_definition_id{}, cb });
								return;
							}
						}
					}
				}
			}
			return;
		} else {
			assert(military::cb_conditions_satisfied(state, n, target, cb));
			result.push_back(possible_cb{ target, dcon::nation_id{}, dcon::national_identity_id{}, dcon::state_definition_id{}, cb });
			return;
		}
	}

	void sort_available_declaration_cbs(std::vector<possible_cb>& result, sys::state& state, dcon::nation_id n, dcon::nation_id target) {
		result.clear();

		static std::vector<dcon::state_instance_id> target_states;
		target_states.clear();
		state_target_list(target_states, state, n, target);

		auto other_cbs = state.world.nation_get_available_cbs(n);
		for(auto& cb : other_cbs) {
			if(cb.target == target) {
				place_instance_in_result(state, result, n, target, cb.cb_type, target_states);
			}
		}
		for(auto cb : state.world.in_cb_type) {
			if((cb.get_type_bits() & military::cb_flag::always) != 0
			&& military::cb_conditions_satisfied(state, n, target, cb)) {
				place_instance_in_result(state, result, n, target, cb, target_states);
			}
		}
		pdqsort(result.begin(), result.end(), [&](possible_cb const& a, possible_cb const& b) {
			/* Actual sort weighting */
			auto a_weight = ai::possible_cb_weight(state, n, a);
			auto b_weight = ai::possible_cb_weight(state, n, b);
			if(a_weight != b_weight)
				return a_weight > b_weight;
			/* Preserve total order */
			if(a.cb != b.cb)
				return a.cb.index() < b.cb.index();
			if(a.target != b.target)
				return a.target.index() < b.target.index();
			if(a.secondary_nation != b.secondary_nation)
				return a.secondary_nation.index() < b.secondary_nation.index();
			if(a.associated_tag != b.associated_tag)
				return a.associated_tag.index() < b.associated_tag.index();
			return a.state_def.index() < b.state_def.index();
		});
	}

	bool get_is_ideal_tws(sys::state& state, dcon::nation_id n, dcon::war_id war, dcon::cb_type_id cb, dcon::state_definition_id sd, dcon::nation_id target, dcon::nation_id secondary_nation, dcon::national_identity_id associated_tag, military::war_role role) {
		auto bits = state.world.cb_type_get_type_bits(cb);
		if((bits & (military::cb_flag::po_annex | military::cb_flag::po_transfer_provinces | military::cb_flag::po_demand_state)) != 0) {
			auto total_count = 0.0f;
			auto occupied = 0.0f;
			if(sd) {
				for(auto prv : state.world.state_definition_get_abstract_state_membership(sd)) {
					if(prv.get_province().get_nation_from_province_ownership() == target) {
						++total_count;
						if(military::get_role(state, war, prv.get_province().get_nation_from_province_control()) == role) {
							++occupied;
						}
					}
				}
			} else if((bits & military::cb_flag::po_annex) != 0) {
				for(auto prv : state.world.nation_get_province_ownership(target)) {
					++total_count;
					if(military::get_role(state, war, prv.get_province().get_nation_from_province_control()) == role) {
						++occupied;
					}
				}
			} else if(auto allowed_states = state.world.cb_type_get_allowed_states(cb); allowed_states) {
				auto from_slot = secondary_nation;
				if(!from_slot)
					from_slot = state.world.national_identity_get_nation_from_identity_holder(associated_tag);
				bool is_lib = (bits & military::cb_flag::po_transfer_provinces) != 0;
				for(auto st : state.world.nation_get_state_ownership(target)) {
					if(trigger::evaluate(state, allowed_states, trigger::to_generic(st.get_state().id), trigger::to_generic(n), is_lib ? trigger::to_generic(from_slot) : trigger::to_generic(n))) {
						province::for_each_province_in_state_instance(state, st.get_state(), [&](dcon::province_id prv) {
							++total_count;
							if(military::get_role(state, war, state.world.province_get_nation_from_province_control(prv)) == role) {
								++occupied;
							}
						});
					}
				}
			}
			if(total_count > 0.0f) {
				auto const fraction = occupied / total_count;
				return fraction >= state.defines.tws_fulfilled_idle_space;
			}
			return false; // not ideal
		}
		return true; // ideal
	}

	/* Adds free (available) CBs to a given war
		@tparam is_favourable Whetever the war is in our favour or not (will need to compute TWS if it's not) */
	template<bool is_favourable>
	void add_ai_cbs_to_war(sys::state& state, dcon::nation_id n, dcon::war_id w) {
		bool is_attacker = military::is_attacker(state, w, n);
		if((is_attacker && military::attackers_have_status_quo_wargoal(state, w))
		|| (!is_attacker && military::defenders_have_status_quo_wargoal(state, w)))
			return;
		/* Must be independent to add wargoals */
		auto const ovr = state.world.nation_get_overlord_as_subject(n);
		if(state.world.overlord_get_ruler(ovr))
			return;

		auto role = military::get_role(state, w, n);
		bool added = false;
		bool is_gw = state.world.war_get_is_great(w);
		do {
			added = false;
			if(state.world.nation_get_diplomatic_points(n) >= state.defines.addwargoal_diplomatic_cost) {
				bool add_unjustified = is_favourable && ai::can_add_unjustified_cbs(state, n, is_gw);
				/* If war is favourable -- be free to add cost-incurring CBs */
				static std::vector<possible_cb> potential;
				potential.clear();
				ai::sort_cbs_for_war(potential, state, n, w, add_unjustified);
				for(auto& p : potential) {
					if(!military::war_goal_would_be_duplicate(state, n, w, p.target, p.cb, p.state_def, p.associated_tag, p.secondary_nation)) {
						// compute -- is this wargoal ideal to add?
						bool will_add = is_favourable
							|| get_is_ideal_tws(state, n, w, p.cb, p.state_def, p.target, p.secondary_nation, p.associated_tag, role);
						if(will_add) {
							assert(command::can_add_war_goal(state, n, w, p.target, p.cb, p.state_def, p.associated_tag, p.secondary_nation));
							command::execute_add_war_goal(state, n, w, p.target, p.cb, p.state_def, p.associated_tag, p.secondary_nation);
							added = true;
							break;
						}
					}
				}
			}
		} while(added);
	}

	void add_free_ai_cbs_to_war(sys::state& state, dcon::nation_id n, dcon::war_id w) {
		ai::add_ai_cbs_to_war<false>(state, n, w);
	}

	dcon::cb_type_id pick_gw_extra_cb_type(sys::state& state, dcon::nation_id from, dcon::nation_id target) {
		static std::vector<dcon::cb_type_id> possibilities;
		possibilities.clear();

		auto const free_infamy = state.defines.badboy_limit - state.world.nation_get_infamy(from);
		for(auto c : state.world.in_cb_type) {
			auto bits = state.world.cb_type_get_type_bits(c);
			if((bits & (military::cb_flag::always | military::cb_flag::is_not_constructing_cb)) != 0)
				continue;
			if((bits & (military::cb_flag::po_demand_state | military::cb_flag::po_annex)) == 0)
				continue;
			if(military::cb_infamy(state, c) * state.defines.gw_justify_cb_badboy_impact >= free_infamy)
				continue;
			if(!military::cb_conditions_satisfied(state, from, target, c))
				continue;
			possibilities.push_back(c);
		}

		if(!possibilities.empty()) {
			return possibilities[rng::reduce(uint32_t(rng::get_random(state, uint32_t((from.index() << 3) ^ target.index()))), uint32_t(possibilities.size()))];
		}
		return dcon::cb_type_id{};
	}

	dcon::nation_id pick_gw_target(sys::state& state, dcon::nation_id from, dcon::war_id w, bool is_attacker) {

		if(is_attacker && military::get_role(state, w, state.world.nation_get_ai_rival(from)) == military::war_role::defender)
			return state.world.nation_get_ai_rival(from);
		if(!is_attacker && military::get_role(state, w, state.world.nation_get_ai_rival(from)) == military::war_role::attacker)
			return state.world.nation_get_ai_rival(from);

		static std::vector<dcon::nation_id> possibilities;
		possibilities.clear();

		for(auto par : state.world.war_get_war_participant(w)) {
			if(par.get_is_attacker() != is_attacker) {
				if(state.world.get_nation_adjacency_by_nation_adjacency_pair(from, par.get_nation()))
					possibilities.push_back(par.get_nation().id);
			}
		}
		if(possibilities.empty()) {
			for(auto par : state.world.war_get_war_participant(w)) {
				if(par.get_is_attacker() != is_attacker) {
					if(nations::is_great_power(state, par.get_nation()))
						possibilities.push_back(par.get_nation().id);
				}
			}
		}
		if(!possibilities.empty()) {
			return possibilities[rng::reduce(uint32_t(rng::get_random(state, uint32_t(from.index() ^ 3))), uint32_t(possibilities.size()))];
		} else {
			return dcon::nation_id{};
		}
	}

	int32_t attacker_peace_cost_plus_potential(sys::state& state, dcon::nation_id n, dcon::war_id w) {
		int32_t cost = military::attacker_peace_cost(state, w);
		bool is_attacker = military::is_attacker(state, w, n);
		if((is_attacker && military::attackers_have_status_quo_wargoal(state, w))
		|| (!is_attacker && military::defenders_have_status_quo_wargoal(state, w)))
			return cost;
		//
		static std::vector<possible_cb> potential;
		potential.clear();
		sort_cbs_for_war(potential, state, n, w, false);
		for(auto& p : potential) {
			if(!military::war_goal_would_be_duplicate(state, n, w, p.target, p.cb, p.state_def, p.associated_tag, p.secondary_nation)) {
				cost += military::peace_cost(state, w, p.cb, n, p.target, p.secondary_nation, p.state_def, p.associated_tag);
			}
		}
		return cost;
	}

	int32_t defender_peace_cost_plus_potential(sys::state& state, dcon::nation_id n, dcon::war_id w) {
		int32_t cost = military::defender_peace_cost(state, w);
		bool is_attacker = military::is_attacker(state, w, n);
		if((is_attacker && military::attackers_have_status_quo_wargoal(state, w))
		|| (!is_attacker && military::defenders_have_status_quo_wargoal(state, w)))
			return cost;
		//
		static std::vector<possible_cb> potential;
		potential.clear();
		sort_cbs_for_war(potential, state, n, w, false);
		for(auto& p : potential) {
			if(!military::war_goal_would_be_duplicate(state, n, w, p.target, p.cb, p.state_def, p.associated_tag, p.secondary_nation)) {
				cost += military::peace_cost(state, w, p.cb, n, p.target, p.secondary_nation, p.state_def, p.associated_tag);
			}
		}
		return cost;
	}

	void add_wg_to_great_war(sys::state& state, dcon::nation_id n, dcon::war_id w) {
		auto rval = rng::get_random(state, n.index() ^ w.index() << 2);
		if((rval & 1) == 0)
			return;

		if(n == state.world.war_get_primary_attacker(w) || n == state.world.war_get_primary_defender(w)) {
			if(((rval >> 1) & 1) == 0) {
				ai::add_ai_cbs_to_war<false>(state, n, w);
			}
		}
		
		auto const totalpop = state.world.nation_get_demographics(n, demographics::total);
		auto const jingoism_perc = totalpop > 0.f
			? state.world.nation_get_demographics(n, demographics::to_key(state, state.culture_definitions.jingoism)) / totalpop
			: 0.0f;
		if(jingoism_perc < state.defines.wargoal_jingoism_requirement * state.defines.gw_wargoal_jingoism_requirement_mod)
			return;

		bool attacker = military::get_role(state, w, n) == military::war_role::attacker;
		auto const spare_ws = attacker ? (military::primary_warscore(state, w) - military::attacker_peace_cost(state, w)) : (-military::primary_warscore(state, w) - military::defender_peace_cost(state, w));
		if(spare_ws < 1.0f)
			return;

		auto const target = pick_gw_target(state, n, w, attacker);
		if(!target)
			return;

		auto const cb = pick_gw_extra_cb_type(state, n, target);
		if(cb) {
			static std::vector<dcon::state_instance_id> target_states;
			target_states.clear();
			state_target_list(target_states, state, n, target);
			static std::vector<possible_cb> result;
			result.clear();
			place_instance_in_result_war(state, result, n, target, w, cb, target_states);
			if(!result.empty() && result[0].target) {
				auto const p = result[0];
				assert(command::can_add_war_goal(state, n, w, p.target, p.cb, p.state_def, p.associated_tag, p.secondary_nation));
				command::execute_add_war_goal(state, n, w, p.target, p.cb, p.state_def, p.associated_tag, p.secondary_nation);
			}
		}
	}

	void add_gw_goals(sys::state& state) {
		for(auto w : state.world.in_war) {
			if(w.get_is_great()) {
				for(auto par : w.get_war_participant()) {
					if(par.get_nation().get_is_player_controlled() == false) {
						add_wg_to_great_war(state, par.get_nation(), w);
					}
				}
			}
		}
	}

	/* Checks that all the cores are occupied by someone else */
	bool has_all_cores_occupied(sys::state& state, dcon::nation_id n) {
		auto i = state.world.nation_get_identity_from_identity_holder(n);
		bool has_owned_cores = false; /* Do we own any of our cores? */
		for(auto const c : state.world.national_identity_get_core(i)) {
			if(c.get_province().get_nation_from_province_control() == n) {
				return false;
			}
			has_owned_cores = has_owned_cores || (c.get_province().get_nation_from_province_ownership() == n);
		}
		/* Must be controlling atleast 1 province, or have owned cores */
		auto pc = state.world.nation_get_province_control(n);
		return has_owned_cores || (pc.begin() != pc.end());
	}

	float war_willingness_factor(int32_t war_duration, bool is_great_war) {
		auto const years = is_great_war ? 4.f : 1.f;
		auto const value = 365.f * years - float(war_duration);
		return value / (365.f * years);
	}
	float war_harshness_factor(int32_t war_duration, bool is_great_war) {
		auto const years = is_great_war ? 8.f : 2.f;
		auto const value = float(war_duration) - 365.f * years;
		return 1.f + std::max(0.f, value / (365.f * years)) * 4.f;
	}

	bool would_surrender_evaluate(sys::state& state, dcon::nation_id n, dcon::war_id w) {
		auto role = military::get_role(state, w, n);
		for(auto par : state.world.war_get_wargoals_attached(w)) {
			if((par.get_wargoal().get_type().get_type_bits() & military::cb_flag::po_annex) != 0
			&& military::get_role(state, w, par.get_wargoal().get_added_by()) != role) {
				auto cap_controller = state.world.province_get_nation_from_province_control(state.world.nation_get_capital(n));
				return military::get_role(state, w, cap_controller) != role;
			}
		}
		return true;
	}

	void make_peace_offers(sys::state& state) {
		auto send_offer_up_to = [&](dcon::nation_id from, dcon::nation_id to, dcon::war_id w, bool attacker, int32_t score_max, bool concession) {
			if(auto off = state.world.nation_get_peace_offer_from_pending_peace_offer(from); off) {
				if(state.world.peace_offer_get_is_crisis_offer(off) == true || state.world.peace_offer_get_war_from_war_settlement(off))
					return; // offer in flight
				state.world.delete_peace_offer(off); // else -- offer has been already resolved and was just pending gc
			}

			assert(command::can_start_peace_offer(state, from, to, w, concession));
			command::execute_start_peace_offer(state, from, to, w, concession);
			auto pending = state.world.nation_get_peace_offer_from_pending_peace_offer(from);
			if(!pending) {
				return;
			}

			if(!concession) { //add our tws-sensitive wargoals :)
				ai::add_ai_cbs_to_war<true>(state, from, w);
			}

			assert(score_max >= 0);
			score_max = std::min(score_max, 100);
			int32_t current_value = 0;
			for(auto const wg : state.world.war_get_wargoals_attached(w)) {
				if((military::is_attacker(state, w, wg.get_wargoal().get_added_by()) == attacker) == !concession) {
					auto const goal_cost = military::peace_cost(state, w, wg.get_wargoal().get_type(), wg.get_wargoal().get_added_by(), wg.get_wargoal().get_target_nation(), wg.get_wargoal().get_secondary_nation(), wg.get_wargoal().get_associated_state(), wg.get_wargoal().get_associated_tag());
					if(current_value + goal_cost <= score_max) {
						current_value += goal_cost;
						state.world.force_create_peace_offer_item(pending, wg.get_wargoal().id);
					} else {
						break;
					}
				}
			}

			assert(command::can_send_peace_offer(state, from));
			command::execute_send_peace_offer(state, from);
		};

		for(auto w : state.world.in_war) {
			if((w.get_primary_attacker().get_is_player_controlled() == false || w.get_primary_defender().get_is_player_controlled() == false)
			&& w.get_primary_attacker().get_owned_province_count() > 0
			&& w.get_primary_defender().get_owned_province_count() > 0) {
				//postpone until military gc does magic
				if(military::get_role(state, w, w.get_primary_attacker()) != military::war_role::attacker)
					continue;
				if(military::get_role(state, w, w.get_primary_defender()) != military::war_role::defender)
					continue;

				auto const war_duration = state.current_date.value - state.world.war_get_start_date(w).value;
				auto const willingness_factor = ai::war_willingness_factor(war_duration, state.world.war_get_is_great(w) || state.world.war_get_is_crisis_war(w));
				auto const harshness_factor = ai::war_harshness_factor(war_duration, state.world.war_get_is_great(w) || state.world.war_get_is_crisis_war(w));
				auto const overall_score = military::primary_warscore(state, w) * harshness_factor;
				if(overall_score >= 0) { // attacker winning
					bool const defender_surrender = has_all_cores_occupied(state, w.get_primary_defender());
					auto const total_po_cost = attacker_peace_cost_plus_potential(state, w.get_primary_attacker(), w);
					if(w.get_primary_attacker().get_is_player_controlled() == false) { // attacker makes offer
						if(defender_surrender || (overall_score >= 100 || (overall_score >= 50 && overall_score >= total_po_cost * 2))) {
							send_offer_up_to(w.get_primary_attacker(), w.get_primary_defender(), w, true, int32_t(overall_score), false);
							continue;
						}
						if(w.get_primary_defender().get_is_player_controlled() == false) {
							if(war_duration >= 365) {
								if(defender_surrender || (overall_score > (total_po_cost - willingness_factor) && (-overall_score / 2 + total_po_cost - willingness_factor) < 0)) {
									send_offer_up_to(w.get_primary_attacker(), w.get_primary_defender(), w, true, int32_t(100), false);
									continue;
								}
							}
						}
					} else if(w.get_primary_defender().get_is_player_controlled() == false) { // defender may surrender
						if(defender_surrender || (overall_score >= 100 || (overall_score >= 50 && overall_score >= total_po_cost * 2))) {
							if(would_surrender_evaluate(state, w.get_primary_defender(), w)) {
								send_offer_up_to(w.get_primary_defender(), w.get_primary_attacker(), w, false, int32_t(overall_score), true);
								continue;
							}
						}
					}
				} else {
					bool const attacker_surrender = has_all_cores_occupied(state, w.get_primary_attacker());
					auto const total_po_cost = defender_peace_cost_plus_potential(state, w.get_primary_defender(), w);
					if(w.get_primary_defender().get_is_player_controlled() == false) { // defender makes offer
						if(attacker_surrender || (overall_score <= -100 || (overall_score <= -50 && overall_score <= -total_po_cost * 2))) {
							send_offer_up_to(w.get_primary_defender(), w.get_primary_attacker(), w, false, int32_t(-overall_score), false);
							continue;
						}
						if(w.get_primary_attacker().get_is_player_controlled() == false) {
							if(war_duration >= 365) {
								if(attacker_surrender  || (-overall_score > (total_po_cost - willingness_factor) && (overall_score / 2 + total_po_cost - willingness_factor) < 0)) {
									send_offer_up_to(w.get_primary_defender(), w.get_primary_attacker(), w, false, int32_t(100), false);
									continue;
								}
							}
						}
					} else if(w.get_primary_attacker().get_is_player_controlled() == false) { // attacker may surrender
						if(attacker_surrender || (overall_score <= -100 || (overall_score <= -50 && overall_score <= -total_po_cost * 2))) {
							if(would_surrender_evaluate(state, w.get_primary_attacker(), w)) {
								send_offer_up_to(w.get_primary_attacker(), w.get_primary_defender(), w, true, int32_t(-overall_score), true);
								continue;
							}
						}
					}
				}
			}
		}
	}

	bool will_accept_peace_offer_value(sys::state& state,
		dcon::nation_id n, dcon::nation_id from,
		dcon::nation_id prime_attacker, dcon::nation_id prime_defender,
		float primary_warscore, float scoreagainst_me,
		bool offer_from_attacker, bool concession,
		int32_t overall_po_value, int32_t my_po_target,
		int32_t target_personal_po_value, int32_t potential_peace_score_against,
		int32_t my_side_against_target, int32_t my_side_peace_cost,
		int32_t war_duration, bool contains_sq)
	{
		bool is_attacking = !offer_from_attacker;
		// TODO: eval for gw and crisis wars
		auto const willingness_factor = ai::war_willingness_factor(war_duration, false);
		auto const harshness_factor = ai::war_harshness_factor(war_duration, false);

		auto const overall_score = primary_warscore * harshness_factor;
		if(concession && overall_score <= -50.0f) {
			return true;
		}

		if(!concession) {
			overall_po_value = -overall_po_value;
		}
		if(overall_po_value < -100)
			return false;

		auto const personal_score_saved = target_personal_po_value - potential_peace_score_against;

		if((prime_attacker == n || prime_defender == n) && (prime_attacker == from || prime_defender == from)) {
			if(overall_score <= -50 && overall_score <= overall_po_value * 2)
				return true;
			if(concession && my_side_peace_cost <= overall_po_value)
				return true; // offer contains everything
			if(war_duration < 365) {
				return false;
			}
			if(overall_score >= 0) {
				if(concession && ((overall_score * 2 - overall_po_value - willingness_factor) < 0))
					return true;
			} else {
				if((overall_score - willingness_factor) <= overall_po_value && (overall_score / 2 - overall_po_value - willingness_factor) < 0)
					return true;
			}
		} else if((prime_attacker == n || prime_defender == n) && concession) {
			if(scoreagainst_me > 50)
				return true;
			if(overall_score < 0.0f) { // we are losing
				if(my_side_against_target - scoreagainst_me <= overall_po_value + personal_score_saved)
					return true;
			} else {
				if(my_side_against_target <= overall_po_value)
					return true;
			}
		} else {
			if(contains_sq)
				return false;
			if(scoreagainst_me > 50 && scoreagainst_me > -overall_po_value * 2)
				return true;

			if(overall_score < 0.0f) { // we are losing
				if(personal_score_saved > 0 && scoreagainst_me + personal_score_saved - my_po_target >= -overall_po_value)
					return true;
			} else { // we are winning
				if(my_po_target > 0 && my_po_target >= overall_po_value)
					return true;
			}
		}

		//will accept anything
		if(has_all_cores_occupied(state, n)) {
			return true;
		}
		return false;
	}

	bool will_accept_peace_offer(sys::state& state, dcon::nation_id n, dcon::nation_id from, dcon::peace_offer_id p) {
		auto w = state.world.peace_offer_get_war_from_war_settlement(p);
		auto prime_attacker = state.world.war_get_primary_attacker(w);
		auto prime_defender = state.world.war_get_primary_defender(w);
		bool is_attacking = military::is_attacker(state, w, n);
		bool contains_sq = false;

		auto const war_duration = state.current_date.value - state.world.war_get_start_date(w).value;
		auto const willingness_factor = ai::war_willingness_factor(war_duration, state.world.war_get_is_great(w) || state.world.war_get_is_crisis_war(w));
		auto const harshness_factor = ai::war_harshness_factor(war_duration, state.world.war_get_is_great(w) || state.world.war_get_is_crisis_war(w));

		auto overall_score = military::primary_warscore(state, w) * harshness_factor;
		if(!is_attacking) {
			overall_score = -overall_score;
		}

		auto concession = state.world.peace_offer_get_is_concession(p);
		if(concession && overall_score <= -50.0f) {
			return true;
		}

		int32_t overall_po_value = 0;
		int32_t personal_po_value = 0;
		int32_t my_po_target = 0;
		for(auto wg : state.world.peace_offer_get_peace_offer_item(p)) {
			auto wg_value = military::peace_cost(state, w, wg.get_wargoal().get_type(), wg.get_wargoal().get_added_by(), wg.get_wargoal().get_target_nation(), wg.get_wargoal().get_secondary_nation(), wg.get_wargoal().get_associated_state(), wg.get_wargoal().get_associated_tag());
			overall_po_value += wg_value;
			if((wg.get_wargoal().get_type().get_type_bits() & military::cb_flag::po_status_quo) != 0) {
				contains_sq = true;
			}
			if(wg.get_wargoal().get_target_nation() == n) {
				personal_po_value += wg_value;
			}
		}
		if(!concession) {
			overall_po_value = -overall_po_value;
		}
		if(overall_po_value < -100) {
			return false;
		}

		int32_t potential_peace_score_against = 0;
		for(auto wg : state.world.war_get_wargoals_attached(w)) {
			if(wg.get_wargoal().get_target_nation() == n || wg.get_wargoal().get_added_by() == n) {
				auto wg_value = military::peace_cost(state, w, wg.get_wargoal().get_type(), wg.get_wargoal().get_added_by(), n, wg.get_wargoal().get_secondary_nation(), wg.get_wargoal().get_associated_state(), wg.get_wargoal().get_associated_tag());
				if(wg.get_wargoal().get_target_nation() == n && (wg.get_wargoal().get_added_by() == from || from == prime_attacker || from == prime_defender)) {
					potential_peace_score_against += wg_value * harshness_factor;
				}
				if(wg.get_wargoal().get_added_by() == n && (wg.get_wargoal().get_target_nation() == from || from == prime_attacker || from == prime_defender)) {
					my_po_target += wg_value * harshness_factor;
				}
			}
		}
		auto personal_score_saved = personal_po_value - potential_peace_score_against;
		if((prime_attacker == n || prime_defender == n) && (prime_attacker == from || prime_defender == from)) {
			if(overall_score <= -50 && overall_score <= overall_po_value * 2)
				return true;
			if(concession && (is_attacking ? military::attacker_peace_cost(state, w) : military::defender_peace_cost(state, w)) <= overall_po_value)
				return true; // offer contains everything
			if(war_duration < 365) {
				return false;
			}
			if(overall_score >= 0) {
				if(concession && ((overall_score * 2 - overall_po_value - willingness_factor) < 0))
					return true;
			} else {
				if((overall_score - willingness_factor) <= overall_po_value && (overall_score / 2 - overall_po_value - willingness_factor) < 0)
					return true;
			}

		} else if((prime_attacker == n || prime_defender == n) && concession) {
			auto scoreagainst_me = military::directed_warscore(state, w, from, n);

			if(scoreagainst_me > 50)
				return true;

			int32_t my_side_against_target = 0;
			for(auto wg : state.world.war_get_wargoals_attached(w)) {
				if(wg.get_wargoal().get_target_nation() == from) {
					auto const wg_value = military::peace_cost(state, w, wg.get_wargoal().get_type(), wg.get_wargoal().get_added_by(), n, wg.get_wargoal().get_secondary_nation(), wg.get_wargoal().get_associated_state(), wg.get_wargoal().get_associated_tag());
					my_side_against_target += wg_value * harshness_factor;
				}
			}

			if(overall_score < 0.0f) { // we are losing
				if(my_side_against_target - scoreagainst_me <= overall_po_value + personal_score_saved) {
					return true;
				}
			} else {
				if(my_side_against_target <= overall_po_value) {
					return true;
				}
			}

		} else {
			if(contains_sq) {
				return false;
			}
			auto scoreagainst_me = military::directed_warscore(state, w, from, n);
			if(scoreagainst_me > 50 && scoreagainst_me > -overall_po_value * 2) {
				return true;
			}
			if(overall_score < 0.0f) { // we are losing
				if(personal_score_saved > 0 && scoreagainst_me + personal_score_saved - my_po_target >= -overall_po_value) {
					return true;
				}
			} else { // we are winning
				if(my_po_target > 0 && my_po_target >= overall_po_value) {
					return true;
				}
			}
		}

		//will accept anything
		if(has_all_cores_occupied(state, n)) {
			return true;
		}
		return false;
	}

	bool naval_supremacy(sys::state& state, dcon::nation_id n, dcon::nation_id target) {
		auto const self_sup = state.world.nation_get_used_naval_supply_points(n);
		auto const ovr = state.world.nation_get_overlord_as_subject(target);
		auto real_target = state.world.overlord_get_ruler(ovr);
		if(!real_target)
			real_target = target;
		if(self_sup <= state.world.nation_get_used_naval_supply_points(real_target))
			return false;
		if(self_sup <= state.world.nation_get_in_sphere_of(real_target).get_used_naval_supply_points())
			return false;

		for(auto a : state.world.nation_get_diplomatic_relation(real_target)) {
			if(a.get_are_allied()) {
				auto const other = a.get_related_nations(0) != real_target ? a.get_related_nations(0) : a.get_related_nations(1);
				if(self_sup <= other.get_used_naval_supply_points())
					return false;
			}
		}
		return true;
	}

	/* Whetever or not this declaration of war would be geopolitically/strategically viable */
	bool decrease_relations_until_can_declare_war(sys::state& state, dcon::nation_id n, dcon::nation_id target) {
		/* Non-tryhard AI behaviour -- will not declare on people with >100 relations */
		if(uint8_t(state.difficulty) <= uint8_t(sys::difficulty_level::normal)) {
			auto rel = state.world.get_diplomatic_relation_by_diplomatic_pair(n, target);
			if(state.world.diplomatic_relation_get_value(rel) < state.defines.make_cb_relation_limit) {
				return true;
			} else {
				/* Decrease relations until at an amount where we can declare war */
				for(uint32_t i = 0; i < 5; ++i) {
					if(state.world.nation_get_diplomatic_points(n) >= state.defines.decreaserelation_diplomatic_cost) {
						assert(command::can_decrease_relations(state, n, target));
						command::execute_decrease_relations(state, n, target);
						/* Now can declare war */
						if(state.world.diplomatic_relation_get_value(rel) < state.defines.make_cb_relation_limit) {
							return true;
						}
					} else {
						break;
					}
				}
			}
			return false;
		}
		return true;
	}

	bool can_make_war_decs(sys::state& state, dcon::nation_id n) {
		if(state.world.nation_get_is_player_controlled(n)
		|| state.world.nation_get_owned_province_count(n) == 0)
			return false;
		if(state.world.nation_get_is_at_war(n)
		|| state.world.nation_get_military_score(n) == 0
		|| state.world.nation_get_diplomatic_points(n) < state.defines.declarewar_diplomatic_cost)
			return false;
		if(auto ll = state.world.nation_get_last_war_loss(n); ll && state.current_date <= (ll + 365 * 5))
			return false;
		if(auto ol = state.world.nation_get_overlord_as_subject(n); state.world.overlord_get_ruler(ol))
			return false;
		return true;
	}

	void make_war_decs(sys::state& state) {
		auto targets = ve::vectorizable_buffer<dcon::nation_id, dcon::nation_id>(state.world.nation_size());
		concurrency::parallel_for(uint32_t(0), state.world.nation_size(), [&](uint32_t i) {
			dcon::nation_id n{ dcon::nation_id::value_base_t(i) };
			if(!ai::can_make_war_decs(state, n))
				return;
			auto const base_strength = estimate_strength(state, n);
			auto best_difference = 2.f;
			//Great powers should look for non-neighbor nations to use their existing wargoals on; helpful for forcing unification/repay debts wars to happen
			if(nations::is_great_power(state, n)) {
				for(auto target : state.world.in_nation) {
					auto const real_target = target.get_overlord_as_subject().get_ruler()
						? target.get_overlord_as_subject().get_ruler() : target;
					if(!ai::can_go_war_with(state, n, real_target, target))
						continue;
					// If it neighbors one of our spheres and we can pathfind to each other's capitals, we don't need naval supremacy to reach this nation
					// Generally here to help Prussia realize it doesn't need a navy to attack Denmark
					auto reachable = false;
					for(auto adj : state.world.nation_get_nation_adjacency(target)) {
						auto const other = adj.get_connected_nations(adj.get_connected_nations(0) == target ? 1 : 0);
						if(other == n) {
							reachable = true;
							break;
						} else if(other.get_in_sphere_of() == n) {
							auto path = province::make_safe_land_path(state, state.world.nation_get_capital(n), state.world.nation_get_capital(other), n);
							if(path.empty()) {
								continue;
							}
							reachable = true;
							break;
						}
					}
					reachable = reachable || ai::naval_supremacy(state, n, target); // Needs navy in this case
					if(reachable) {
						auto const str_difference = ai::war_weight_potential_target(state, n, real_target, base_strength);
						if(str_difference > best_difference) {
							best_difference = str_difference;
							targets.set(n, target.id);
						}
					}
				}
				// try some random coastal nations
				if(state.world.nation_get_central_ports(n) > 0) {
					for(uint32_t j = 0; j < 6; ++j) {
						auto const rvalue = rng::get_random(state, uint32_t((n.index() << 3) + j));
						auto const reduced_value = rng::reduce(uint32_t(rvalue), state.world.nation_size());
						dcon::nation_id other{ dcon::nation_id::value_base_t(reduced_value) };
						auto const real_target = fatten(state.world, other).get_overlord_as_subject().get_ruler() ? fatten(state.world, other).get_overlord_as_subject().get_ruler() : fatten(state.world, other);
						if(!ai::can_go_war_with(state, n, real_target, other))
							continue;
						if(state.world.nation_get_central_ports(other) == 0 || state.world.nation_get_central_ports(real_target) == 0)
							continue;
						if(!state.world.get_nation_adjacency_by_nation_adjacency_pair(n, other) && !ai::naval_supremacy(state, n, other))
							continue;
						auto const str_difference = ai::war_weight_potential_target(state, n, real_target, base_strength);
						if(str_difference > best_difference) {
							best_difference = str_difference;
							targets.set(n, other);
						}
					}
				}
			} else {
				/* Other nations will just look for adjacent ones */
				for(auto adj : state.world.nation_get_nation_adjacency(n)) {
					auto const other = adj.get_connected_nations(0) != n ? adj.get_connected_nations(0) : adj.get_connected_nations(1);
					auto const real_target = other.get_overlord_as_subject().get_ruler() ? other.get_overlord_as_subject().get_ruler() : other;
					if(!ai::can_go_war_with(state, n, real_target, other))
						continue;
					if(!state.world.get_nation_adjacency_by_nation_adjacency_pair(n, other)
					&& !ai::naval_supremacy(state, n, other))
						continue;
					auto const str_difference = ai::war_weight_potential_target(state, n, real_target, base_strength);
					if(str_difference > best_difference) {
						best_difference = str_difference;
						targets.set(n, other.id);
					}
				}
			}
		});
		for(auto n : state.world.in_nation) {
			if(n.get_is_at_war() == false && targets.get(n)) {
				auto const t = targets.get(n);
				static std::vector<possible_cb> potential;
				potential.clear();
				sort_available_declaration_cbs(potential, state, n, t);
				if(!potential.empty()) {
					/* If not in hard/very hard, AI needs to decrease relations to declare war :) */
					if(ai::decrease_relations_until_can_declare_war(state, n, t)) {
						assert(command::can_declare_war(state, n, t, potential[0].cb, potential[0].state_def, potential[0].associated_tag, potential[0].secondary_nation));
						command::execute_declare_war(state, n, t, potential[0].cb, potential[0].state_def, potential[0].associated_tag, potential[0].secondary_nation, true);
					}
				}
			}
		}

		/* Mobilize nations */
		for(auto const n : state.world.in_nation) {
			if(!n.get_is_player_controlled() && n.get_owned_province_count() > 0) {
				bool will_mob = false;
				if(n.get_ai_is_threatened()) { //threatened -- someone can use cb against us
					for(auto other : state.world.in_nation) {
						if(other.get_is_mobilized() && military::can_use_cb_against(state, other, n)) {
							will_mob = true;
							break;
						}
					}
				}
				if(n.get_is_at_war()) {
					will_mob = n.get_ai_is_threatened(); //is our existence threatened?
				}
				if(!will_mob && n.get_is_at_war() && n.get_is_mobilized()) {
					will_mob = true; //don't demob while at war -- we already mobilized for a reason
				}
				will_mob |= (n.get_ai_rival().get_is_mobilized()); // mobilize if our rival does too
				// can we even mob? (mobbing in some mods causes debuffs)
				int32_t possible_sum = 0;
				for(auto po : n.get_province_ownership_as_nation()) {
					possible_sum += military::mobilized_regiments_possible_from_province(state, po.get_province());
					if(possible_sum > 0) {
						break;
					}
				}
				if(possible_sum == 0) {
					will_mob = false;
				}
				if(will_mob && !n.get_is_mobilized()) {
					military::start_mobilization(state, n);
				} else if(!will_mob && n.get_is_mobilized()) {
					military::end_mobilization(state, n);
				}
			}
		}
	}

	void update_budget(sys::state& state) {
		state.world.execute_parallel_over_nation([&](auto ids) {
			// read phase -- daily income
			auto const total_income = state.world.nation_get_total_rich_income(ids)
				+ state.world.nation_get_total_middle_income(ids)
				+ state.world.nation_get_total_poor_income(ids);
			/* We need to have 60 days of income worth in our treasury, additionally, have not lost money in the previous day */
			auto const treasury = state.world.nation_get_stockpiles(ids, economy::money);
			auto const was_profitable = ((total_income * 365.f) <= treasury)
				|| (state.world.nation_get_last_treasury(ids) <= treasury && (total_income * 120.f) <= treasury);
			auto const subopt_admin = state.world.nation_get_administrative_efficiency(ids) < 0.95f || state.world.nation_get_administrative_spending(ids) < 10;
			auto const at_war = state.world.nation_get_is_at_war(ids);
			auto const at_risk = state.world.nation_get_ai_is_threatened(ids);

			auto const lt_adm = state.world.nation_get_administrative_spending(ids) <= 20;
			auto const lt_edu = state.world.nation_get_education_spending(ids) <= 55;
			auto const lt_mil = state.world.nation_get_military_spending(ids) <= 55;
			auto const lt_con = state.world.nation_get_construction_spending(ids) <= 55;

			auto const t_step = ve::int_vector(2); //tiny
			auto const s_step = ve::int_vector(5); //small
			auto const m_step = ve::int_vector(7); //medium
			auto const l_step = ve::int_vector(10); //large

			// these 3 are always kept at >15%, always
			auto const new_adm = ve::select(lt_adm, ve::int_vector(20), state.world.nation_get_administrative_spending(ids) +  ve::select(was_profitable, t_step, ve::select(subopt_admin, l_step, -s_step)));
			auto const new_edu = ve::select(lt_edu, ve::int_vector(55), state.world.nation_get_education_spending(ids) + ve::select(was_profitable, s_step, -s_step));
			auto const new_mil = ve::select(lt_mil, ve::int_vector(55), state.world.nation_get_military_spending(ids) + ve::select(was_profitable, s_step, ve::select(at_risk, -t_step, -s_step)));
			
			auto const new_social = state.world.nation_get_social_spending(ids) + ve::select(was_profitable, s_step, -t_step);
			auto const new_dominv = state.world.nation_get_domestic_investment_spending(ids) + ve::select(was_profitable, s_step, -t_step);

			// large/medium increases, small decreases
			auto const new_con = ve::select(lt_con, ve::int_vector(55), state.world.nation_get_construction_spending(ids) + ve::select(was_profitable, m_step, -t_step));
			auto const new_land = ve::select(at_war, ve::int_vector(100), state.world.nation_get_land_spending(ids) + ve::select(was_profitable, m_step, ve::select(at_risk, -s_step, -m_step)));
			auto const new_navy = state.world.nation_get_naval_spending(ids) + ve::select(was_profitable, m_step, ve::select(at_war, l_step, ve::select(at_risk, -s_step, -m_step)));

			// large increases, small decreases
			auto const rules = state.world.nation_get_combined_issue_rules(ids);
			auto const is_lf = ((rules & issue_rule::pop_build_factory) != 0 || (rules & issue_rule::pop_expand_factory) != 0);

			auto const new_tax_r = state.world.nation_get_rich_tax(ids) + ve::select(was_profitable, -ve::select(is_lf, l_step, s_step), ve::select(is_lf, t_step, s_step));
			auto const new_tax_m = state.world.nation_get_middle_tax(ids) + ve::select(was_profitable, -s_step, m_step);
			auto const new_tax_p = state.world.nation_get_poor_tax(ids) + ve::select(was_profitable, -s_step, l_step);

			auto const raise_tariffs = (new_tax_r >= 95 || new_tax_m >= 95 || new_tax_p >= 95) && !was_profitable;
			auto const new_tariff = state.world.nation_get_tariffs(ids) + ve::select(raise_tariffs, s_step, -s_step);

			// apply (write phase)
			auto const filter = !state.world.nation_get_is_player_controlled(ids) && state.world.nation_get_owned_province_count(ids) > 0;
			state.world.nation_set_administrative_spending(ids, ve::select(filter, new_adm, state.world.nation_get_administrative_spending(ids)));
			state.world.nation_set_education_spending(ids, ve::select(filter, new_edu, state.world.nation_get_education_spending(ids)));
			state.world.nation_set_domestic_investment_spending(ids, ve::select(filter, new_dominv, state.world.nation_get_domestic_investment_spending(ids)));
			state.world.nation_set_military_spending(ids, ve::select(filter, new_mil, state.world.nation_get_military_spending(ids)));
			state.world.nation_set_construction_spending(ids, ve::select(filter, new_con, state.world.nation_get_construction_spending(ids)));
			state.world.nation_set_land_spending(ids, ve::select(filter, new_land, state.world.nation_get_land_spending(ids)));
			state.world.nation_set_naval_spending(ids, ve::select(filter, new_navy, state.world.nation_get_naval_spending(ids)));
			state.world.nation_set_social_spending(ids, ve::select(filter, new_social, state.world.nation_get_social_spending(ids)));
			state.world.nation_set_rich_tax(ids, ve::select(filter, new_tax_r, state.world.nation_get_rich_tax(ids)));
			state.world.nation_set_middle_tax(ids, ve::select(filter, new_tax_m, state.world.nation_get_middle_tax(ids)));
			state.world.nation_set_poor_tax(ids, ve::select(filter, new_tax_p, state.world.nation_get_poor_tax(ids)));
			state.world.nation_set_tariffs(ids, ve::select(filter, new_tariff, state.world.nation_get_tariffs(ids)));
			// filter to boundaries
			ve::apply([&](dcon::nation_id n) {
				economy::bound_budget_settings(state, n);
			}, ids);
		});
	}

	void remove_ai_data(sys::state& state, dcon::nation_id n) {
		for(auto ar : state.world.nation_get_army_control(n)) {
			ar.get_army().set_ai_activity(0);
		ar.get_army().set_ai_province(dcon::province_id{});
		}
		for(auto v : state.world.nation_get_navy_control(n)) {
			v.get_navy().set_ai_activity(0);
		}
	}

	bool unit_on_ai_control(sys::state& state, dcon::army_id a) {
		auto fat_id = dcon::fatten(state.world, a);
		return fat_id.get_controller_from_army_control().get_is_player_controlled()
		? fat_id.get_is_ai_controlled()
		: true;
	}
	/*bool unit_on_ai_control(sys::state& state, dcon::navy_id a) {
		auto fat_id = dcon::fatten(state.world, a);
		return fat_id.get_controller_from_navy_control().get_is_player_controlled()
		? fat_id.get_is_ai_controlled()
		: true;
	}*/

	void update_ships(sys::state& state) {
		static std::vector<dcon::ship_id> to_delete;
		to_delete.clear();
		for(auto n : state.world.in_nation) {
			if(n.get_is_player_controlled()) {
				continue;
			}
			if(n.get_is_at_war() == false && nations::is_landlocked(state, n)) {
				for(auto v : n.get_navy_control()) {
					if(!v.get_navy().get_battle_from_navy_battle_participation()) {
						for(auto shp : v.get_navy().get_navy_membership()) {
							to_delete.push_back(shp.get_ship().id);
						}
					}
				}
			} else if(n.get_is_at_war() == false) {
				dcon::unit_type_id best_transport;
				dcon::unit_type_id best_light;
				dcon::unit_type_id best_big;
				for(uint32_t i = 2; i < state.military_definitions.unit_base_definitions.size(); ++i) {
					dcon::unit_type_id j{ dcon::unit_type_id::value_base_t(i) };
					if(!n.get_active_unit(j) && !state.military_definitions.unit_base_definitions[j].active) {
						continue;
					}
					if(state.military_definitions.unit_base_definitions[j].type == military::unit_type::transport) {
						if(!best_transport || state.military_definitions.unit_base_definitions[best_transport].defence_or_hull < state.military_definitions.unit_base_definitions[j].defence_or_hull) {
							best_transport = j;
						}
					} else if(state.military_definitions.unit_base_definitions[j].type == military::unit_type::light_ship) {
						if(!best_light || state.military_definitions.unit_base_definitions[best_light].defence_or_hull < state.military_definitions.unit_base_definitions[j].defence_or_hull) {
							best_light = j;
						}
					} else if(state.military_definitions.unit_base_definitions[j].type == military::unit_type::big_ship) {
						if(!best_big || state.military_definitions.unit_base_definitions[best_big].defence_or_hull < state.military_definitions.unit_base_definitions[j].defence_or_hull) {
							best_big = j;
						}
					}
				}
				for(auto v : n.get_navy_control()) {
					if(!v.get_navy().get_battle_from_navy_battle_participation()) {
						auto trange = v.get_navy().get_army_transport();
						bool transporting = trange.begin() != trange.end();

						for(auto shp : v.get_navy().get_navy_membership()) {
							auto type = shp.get_ship().get_type();

							if(state.military_definitions.unit_base_definitions[type].type == military::unit_type::transport && !transporting) {
								if(best_transport && type != best_transport) {
									to_delete.push_back(shp.get_ship().id);
								}
							} else if(state.military_definitions.unit_base_definitions[type].type == military::unit_type::light_ship) {
								if(best_light && type != best_light) {
									to_delete.push_back(shp.get_ship().id);
								}
							} else if(state.military_definitions.unit_base_definitions[type].type == military::unit_type::big_ship) {
								if(best_big && type != best_big) {
									to_delete.push_back(shp.get_ship().id);
								}
							}
						}
					}
				}
			}
		}
		for(auto s : to_delete) {
			state.world.delete_ship(s);
		}
	}

	void build_ships(sys::state& state) {
		for(auto n : state.world.in_nation) {
			if(!n.get_is_player_controlled() && n.get_province_naval_construction().begin() == n.get_province_naval_construction().end()) {
				auto disarm = n.get_disarmed_until();
				if(disarm && state.current_date < disarm)
				continue;

				dcon::unit_type_id best_transport;
				dcon::unit_type_id best_light;
				dcon::unit_type_id best_big;

				for(uint32_t i = 2; i < state.military_definitions.unit_base_definitions.size(); ++i) {
				dcon::unit_type_id j{ dcon::unit_type_id::value_base_t(i) };
					if(!n.get_active_unit(j) && !state.military_definitions.unit_base_definitions[j].active)
					continue;

					if(state.military_definitions.unit_base_definitions[j].type == military::unit_type::transport) {
						if(!best_transport || state.military_definitions.unit_base_definitions[best_transport].defence_or_hull < state.military_definitions.unit_base_definitions[j].defence_or_hull) {
							best_transport = j;
						}
					} else if(state.military_definitions.unit_base_definitions[j].type == military::unit_type::light_ship) {
						if(!best_light || state.military_definitions.unit_base_definitions[best_light].defence_or_hull < state.military_definitions.unit_base_definitions[j].defence_or_hull) {
							best_light = j;
						}
					} else if(state.military_definitions.unit_base_definitions[j].type == military::unit_type::big_ship) {
						if(!best_big || state.military_definitions.unit_base_definitions[best_big].defence_or_hull < state.military_definitions.unit_base_definitions[j].defence_or_hull) {
							best_big = j;
						}
					}
				}

				int32_t num_transports = 0;
				int32_t fleet_cap_in_transports = 0;
				int32_t fleet_cap_in_small = 0;
				int32_t fleet_cap_in_big = 0;

				for(auto v : n.get_navy_control()) {
					for(auto s : v.get_navy().get_navy_membership()) {
						auto type = s.get_ship().get_type();
						if(state.military_definitions.unit_base_definitions[type].type == military::unit_type::transport) {
							++num_transports;
							fleet_cap_in_transports += state.military_definitions.unit_base_definitions[type].supply_consumption_score;
						} else if(state.military_definitions.unit_base_definitions[type].type == military::unit_type::big_ship) {
							fleet_cap_in_big += state.military_definitions.unit_base_definitions[type].supply_consumption_score;
						} else if(state.military_definitions.unit_base_definitions[type].type == military::unit_type::light_ship) {
							fleet_cap_in_small += state.military_definitions.unit_base_definitions[type].supply_consumption_score;
						}
					}
				}

				static std::vector<dcon::province_id> owned_ports;
				owned_ports.clear();
				for(auto p : n.get_province_ownership()) {
					if(p.get_province().get_is_coast() && p.get_province().get_nation_from_province_control() == n) {
						owned_ports.push_back(p.get_province().id);
					}
				}
				auto cap = n.get_capital().id;
				pdqsort(owned_ports.begin(), owned_ports.end(), [&](dcon::province_id a, dcon::province_id b) {
					auto a_dist = province::sorting_distance(state, a, cap);
					auto b_dist = province::sorting_distance(state, b, cap);
					if(a_dist != b_dist)
						return a_dist < b_dist;
					return a.index() < b.index();
				});

				int32_t constructing_fleet_cap = 0;
				if(best_transport) {
					if(fleet_cap_in_transports * 3 < n.get_naval_supply_points()) {
						auto overseas_allowed = state.military_definitions.unit_base_definitions[best_transport].can_build_overseas;
						auto level_req = state.military_definitions.unit_base_definitions[best_transport].min_port_level;
						auto supply_pts = state.military_definitions.unit_base_definitions[best_transport].supply_consumption_score;

						for(uint32_t j = 0; j < owned_ports.size() && (fleet_cap_in_transports + constructing_fleet_cap) * 3 < n.get_naval_supply_points(); ++j) {
							if((overseas_allowed || !province::is_overseas(state, owned_ports[j]))
							&& state.world.province_get_building_level(owned_ports[j], state.economy_definitions.naval_base_building) >= level_req) {
								assert(command::can_start_naval_unit_construction(state, n, owned_ports[j], best_transport));
								auto c = fatten(state.world, state.world.try_create_province_naval_construction(owned_ports[j], n));
								c.set_remaining_construction_time(state.military_definitions.unit_base_definitions[best_transport].build_time);
								c.set_type(best_transport);
								constructing_fleet_cap += supply_pts;
							}
						}
					} else if(num_transports < 10) {
						auto overseas_allowed = state.military_definitions.unit_base_definitions[best_transport].can_build_overseas;
						auto level_req = state.military_definitions.unit_base_definitions[best_transport].min_port_level;
						auto supply_pts = state.military_definitions.unit_base_definitions[best_transport].supply_consumption_score;

						for(uint32_t j = 0; j < owned_ports.size() && num_transports < 10; ++j) {
							if((overseas_allowed || !province::is_overseas(state, owned_ports[j]))
							&& state.world.province_get_building_level(owned_ports[j], state.economy_definitions.naval_base_building) >= level_req) {
								assert(command::can_start_naval_unit_construction(state, n, owned_ports[j], best_transport));
								auto c = fatten(state.world, state.world.try_create_province_naval_construction(owned_ports[j], n));
								c.set_remaining_construction_time(state.military_definitions.unit_base_definitions[best_transport].build_time);
								c.set_type(best_transport);
								++num_transports;
								constructing_fleet_cap += supply_pts;
							}
						}
					}
				}

				int32_t used_points = n.get_used_naval_supply_points();
				auto rem_free = n.get_naval_supply_points() - (fleet_cap_in_transports + fleet_cap_in_small + fleet_cap_in_big + constructing_fleet_cap);
				fleet_cap_in_small = std::max(fleet_cap_in_small, 1);
				fleet_cap_in_big = std::max(fleet_cap_in_big, 1);

				auto free_big_points = best_light ? rem_free * fleet_cap_in_small / (fleet_cap_in_small + fleet_cap_in_big) : rem_free;
				auto free_small_points = best_big ? rem_free * fleet_cap_in_big / (fleet_cap_in_small + fleet_cap_in_big) : rem_free;

				if(best_light) {
					auto overseas_allowed = state.military_definitions.unit_base_definitions[best_light].can_build_overseas;
					auto level_req = state.military_definitions.unit_base_definitions[best_light].min_port_level;
					auto supply_pts = state.military_definitions.unit_base_definitions[best_light].supply_consumption_score;

					for(uint32_t j = 0; j < owned_ports.size() && supply_pts <= free_small_points; ++j) {
						if((overseas_allowed || !province::is_overseas(state, owned_ports[j]))
						&& state.world.province_get_building_level(owned_ports[j], state.economy_definitions.naval_base_building) >= level_req) {
							assert(command::can_start_naval_unit_construction(state, n, owned_ports[j], best_light));
							auto c = fatten(state.world, state.world.try_create_province_naval_construction(owned_ports[j], n));
							c.set_remaining_construction_time(state.military_definitions.unit_base_definitions[best_light].build_time);
							c.set_type(best_light);
							free_small_points -= supply_pts;
						}
					}
				}
				if(best_big) {
					auto overseas_allowed = state.military_definitions.unit_base_definitions[best_big].can_build_overseas;
					auto level_req = state.military_definitions.unit_base_definitions[best_big].min_port_level;
					auto supply_pts = state.military_definitions.unit_base_definitions[best_big].supply_consumption_score;

					for(uint32_t j = 0; j < owned_ports.size() && supply_pts <= free_big_points; ++j) {
						if((overseas_allowed || !province::is_overseas(state, owned_ports[j]))
						&& state.world.province_get_building_level(owned_ports[j], state.economy_definitions.naval_base_building) >= level_req) {
							assert(command::can_start_naval_unit_construction(state, n, owned_ports[j], best_big));
							auto c = fatten(state.world, state.world.try_create_province_naval_construction(owned_ports[j], n));
							c.set_remaining_construction_time(state.military_definitions.unit_base_definitions[best_big].build_time);
							c.set_type(best_big);
							free_big_points -= supply_pts;
						}
					}
				}
			}
		}
	}

	dcon::province_id get_home_port(sys::state& state, dcon::nation_id n) {
		auto cap = state.world.nation_get_capital(n);
		int32_t max_level = -1;
		dcon::province_id result;
		float current_distance = 1.0f;
		for(auto p : state.world.nation_get_province_ownership(n)) {
			if(p.get_province().get_is_coast() && p.get_province().get_nation_from_province_control() == n) {
				if(p.get_province().get_building_level(state.economy_definitions.naval_base_building) > max_level) {
					max_level = p.get_province().get_building_level(state.economy_definitions.naval_base_building);
					result = p.get_province();
					current_distance = province::sorting_distance(state, cap, p.get_province());
				} else if(result && p.get_province().get_building_level(state.economy_definitions.naval_base_building) == max_level && province::sorting_distance(state, cap, p.get_province()) < current_distance) {
					current_distance = province::sorting_distance(state, cap, p.get_province());
					result = p.get_province();
				}
			}
		}
		return result;
	}

	void refresh_home_ports(sys::state& state) {
		for(auto n : state.world.in_nation) {
			if(!n.get_is_player_controlled() && n.get_owned_province_count() > 0) {
				n.set_ai_home_port(get_home_port(state, n));
			}
		}
	}

	void daily_cleanup(sys::state& state) {

	}

	bool navy_needs_repair(sys::state& state, dcon::navy_id n) {
		for(auto shp : state.world.navy_get_navy_membership(n)) {
			if(shp.get_ship().get_strength() < 0.5f) {
				return true;
			}
			//if(shp.get_ship().get_org() < 0.75f * max_org)
			//	return true;
		}
		return false;
	}

	bool naval_advantage(sys::state& state, dcon::nation_id n) {
		for(auto par : state.world.nation_get_war_participant(n)) {
			for(auto other : par.get_war().get_war_participant()) {
				if(other.get_is_attacker() != par.get_is_attacker()) {
					if(other.get_nation().get_used_naval_supply_points() > state.world.nation_get_used_naval_supply_points(n)) {
						return false;
					}
				}
			}
		}
		return true;
	}

	void send_fleet_home(sys::state& state, dcon::navy_id n, fleet_activity moving_status = fleet_activity::returning_to_base, fleet_activity at_base = fleet_activity::idle) {
		auto v = fatten(state.world, n);
		auto home_port = v.get_controller_from_navy_control().get_ai_home_port();
		if(v.get_location_from_navy_location() == home_port) {
			v.set_ai_activity(uint8_t(at_base));
		} else if(!home_port) {
			v.set_ai_activity(uint8_t(fleet_activity::unspecified));
		} else if(auto naval_path = province::make_naval_path(state, v.get_location_from_navy_location(), home_port); naval_path.size() > 0) {
			auto new_size = uint32_t(naval_path.size());
			auto existing_path = v.get_path();
			existing_path.resize(new_size);

			for(uint32_t i = 0; i < new_size; ++i) {
				existing_path[i] = naval_path[i];
			}
			v.set_arrival_time(military::arrival_time_to(state, v, naval_path.back()));
			v.set_ai_activity(uint8_t(moving_status));
		} else {
			v.set_ai_activity(uint8_t(fleet_activity::unspecified));
		}
	}

	bool set_fleet_target(sys::state& state, dcon::nation_id n, dcon::province_id start, dcon::navy_id for_navy) {
		dcon::province_id result;
		float closest = 0.0f;
		for(auto par : state.world.nation_get_war_participant(n)) {
			for(auto other : par.get_war().get_war_participant()) {
				if(other.get_is_attacker() != par.get_is_attacker()) {
					for(auto nv : other.get_nation().get_navy_control()) {
						auto loc = nv.get_navy().get_location_from_navy_location();
						auto dist = province::sorting_distance(state, start, loc);
						if(!result || dist < closest) {
							if(loc.id.index() < state.province_definitions.first_sea_province.index()) {
								result = loc.get_port_to();
							} else {
								result = loc;
							}
							closest = dist;
						}
					}
				}
			}
		}

		if(result == start) {
			return true;
		}

		if(result) {
			auto existing_path = state.world.navy_get_path(for_navy);
			auto path = province::make_naval_path(state, start, result);
			if(path.size() > 0) {
				auto new_size = std::min(uint32_t(path.size()), uint32_t(4));
				existing_path.resize(new_size);
				for(uint32_t i = new_size; i-- > 0;) {
					assert(path[path.size() - 1 - i]);
					existing_path[new_size - 1 - i] = path[path.size() - 1 - i];
				}
				state.world.navy_set_arrival_time(for_navy, military::arrival_time_to(state, for_navy, path.back()));
				state.world.navy_set_ai_activity(for_navy, uint8_t(fleet_activity::attacking));
				return true;
			} else {
				return false;
			}
		} else {
			return false;
		}
	}

	void unload_units_from_transport(sys::state& state, dcon::navy_id n) {
		auto transported_armies = state.world.navy_get_army_transport(n);
		auto location = state.world.navy_get_location_from_navy_location(n);

		for(auto ar : transported_armies) {
			auto path = province::make_land_path(state, location, ar.get_army().get_ai_province(), ar.get_army().get_controller_from_army_control(), ar.get_army());
			if(path.size() > 0) {
				auto existing_path = ar.get_army().get_path();
				auto new_size = uint32_t(path.size());
				existing_path.resize(new_size);

				for(uint32_t i = 0; i < new_size; ++i) {
					assert(path[i]);
					existing_path[i] = path[i];
				}
				ar.get_army().set_arrival_time(military::arrival_time_to(state, ar.get_army(), path.back()));
				ar.get_army().set_dig_in(0);
				auto activity = army_activity(ar.get_army().get_ai_activity());
				if(activity == army_activity::transport_guard) {
					ar.get_army().set_ai_activity(uint8_t(army_activity::on_guard));
				} else if(activity == army_activity::transport_attack) {
					ar.get_army().set_ai_activity(uint8_t(army_activity::attack_gathered));
				}
			}
		}

		state.world.navy_set_ai_activity(n, uint8_t(fleet_activity::unloading));
	}

	bool merge_fleet(sys::state& state, dcon::navy_id n, dcon::province_id p, dcon::nation_id owner) {
		auto merge_target = [&]() {
			dcon::navy_id largest;
			int32_t largest_size = 0;
			for(auto on : state.world.province_get_navy_location(p)) {
				if(on.get_navy() != n && on.get_navy().get_controller_from_navy_control() == owner) {
					auto other_mem = on.get_navy().get_navy_membership();
					if(auto sz = int32_t(other_mem.end() - other_mem.begin()); sz > largest_size) {
						largest =  on.get_navy().id;
						largest_size = sz;
					}
				}
			}
			return largest;
		}();

		if(!merge_target) {
			return false;
		}

		auto regs = state.world.navy_get_navy_membership(n);
		while(regs.begin() != regs.end()) {
			auto reg = (*regs.begin()).get_ship();
			reg.set_navy_from_navy_membership(merge_target);
		}

		auto transported = state.world.navy_get_army_transport(n);
		while(transported.begin() != transported.end()) {
			auto arm = (*transported.begin()).get_army();
			arm.set_navy_from_army_transport(merge_target);
		}
		return true;
	}

	void pickup_idle_ships(sys::state& state) {
		for(auto n : state.world.in_navy) {
			if(n.get_battle_from_navy_battle_participation()) {
				continue;
			}
			if(n.get_arrival_time()) {
				continue;
			}

			auto owner = n.get_controller_from_navy_control();

			if(!owner || owner.get_is_player_controlled() || owner.get_owned_province_count() == 0) {
				continue;
			}

			auto home_port = state.world.nation_get_ai_home_port(owner);
			if(!home_port) {
				continue;
			}

			auto location = state.world.navy_get_location_from_navy_location(n);
			auto activity = fleet_activity(state.world.navy_get_ai_activity(n));

			switch(activity) {
			case fleet_activity::unspecified:
				if(location == home_port) {
					if(!merge_fleet(state, n, location, owner))
						state.world.navy_set_ai_activity(n, uint8_t(fleet_activity::idle));
				} else {
					// move to home port to merge
					send_fleet_home(state, n, fleet_activity::merging);
				}
				break;
			case fleet_activity::boarding:
			{
				bool all_loaded = true;
				for(auto ar : state.world.nation_get_army_control(owner)) {
					if(ar.get_army().get_ai_activity() == uint8_t(army_activity::transport_guard) || ar.get_army().get_ai_activity() == uint8_t(army_activity::transport_attack)) {
						if(ar.get_army().get_navy_from_army_transport() != n)
							all_loaded = false;
					}
				}

				if(all_loaded) {
					auto transporting_range = n.get_army_transport();
					if(transporting_range.begin() == transporting_range.end()) { // failed to pick up troops
						send_fleet_home(state, n);
					} else {
						auto transported_dest = (*(transporting_range.begin())).get_army().get_ai_province();

						// move to closest port or closest off_shore
						if(transported_dest.get_is_coast()) {
							auto target_prov = transported_dest.id;
							if(!province::has_naval_access_to_province(state, owner, target_prov)) {
								target_prov = state.world.province_get_port_to(target_prov);
							}
							auto naval_path = province::make_naval_path(state, location, target_prov);

							auto existing_path = n.get_path();
							auto new_size = uint32_t(naval_path.size());
							existing_path.resize(new_size);

							for(uint32_t k = 0; k < new_size; ++k) {
								existing_path[k] = naval_path[k];
							}
							if(new_size > 0) {
								n.set_arrival_time(military::arrival_time_to(state, n, naval_path.back()));
								n.set_ai_activity(uint8_t(fleet_activity::transporting));
							} else {
								n.set_arrival_time(sys::date{});
								send_fleet_home(state, n);
							}

						} else if(auto path = province::make_path_to_nearest_coast(state, owner, transported_dest); path.empty()) {
							send_fleet_home(state, n);
						} else {
							auto target_prov = path.front();
							if(!province::has_naval_access_to_province(state, owner, target_prov)) {
								target_prov = state.world.province_get_port_to(target_prov);
							}
							auto naval_path = province::make_naval_path(state, location, target_prov);

							auto existing_path = n.get_path();
							auto new_size = uint32_t(naval_path.size());
							existing_path.resize(new_size);

							for(uint32_t k = 0; k < new_size; ++k) {
								existing_path[k] = naval_path[k];
							}
							if(new_size > 0) {
								n.set_arrival_time(military::arrival_time_to(state, n, naval_path.back()));
								n.set_ai_activity(uint8_t(fleet_activity::transporting));
							} else {
								n.set_arrival_time(sys::date{});
								send_fleet_home(state, n);
							}
						}
					}
				}
			}
			break;
			case fleet_activity::transporting:
				unload_units_from_transport(state, n);
				break;
			case fleet_activity::failed_transport:
				if(location == home_port) {
					if(!merge_fleet(state, n, location, owner))
						state.world.navy_set_ai_activity(n, uint8_t(fleet_activity::idle));
				} else if(home_port) {
					auto existing_path = state.world.navy_get_path(n);
					auto path = province::make_naval_path(state, location, home_port);
					if(path.size() > 0) {
						auto new_size = uint32_t(path.size());
						existing_path.resize(new_size);

						for(uint32_t i = 0; i < new_size; ++i) {
							assert(path[i]);
							existing_path[i] = path[i];
						}
						state.world.navy_set_arrival_time(n, military::arrival_time_to(state, n, path.back()));
					}
				}
				break;
			case fleet_activity::returning_to_base:
				if(location == home_port) {
					if(!merge_fleet(state, n, location, owner))
						state.world.navy_set_ai_activity(n, uint8_t(fleet_activity::idle));
				} else {
					send_fleet_home(state, n);
				}
				break;
			case fleet_activity::attacking:
				if(state.world.nation_get_is_at_war(owner) == false) {
					send_fleet_home(state, n);
				} else if(navy_needs_repair(state, n)) {
					send_fleet_home(state, n);
				} else {
					if(naval_advantage(state, owner) && set_fleet_target(state, owner, state.world.navy_get_location_from_navy_location(n), n)) {
						// do nothing -- target set successfully
					} else {
						send_fleet_home(state, n);
					}
				}
				break;
			case fleet_activity::merging:
				if(location == home_port) {
					if(!merge_fleet(state, n, location, owner)) {
						state.world.navy_set_ai_activity(n, uint8_t(fleet_activity::idle));
					}
				} else {
					send_fleet_home(state, n);
				}
				break;
			case fleet_activity::idle:
				if(location != home_port) {
					state.world.navy_set_ai_activity(n, uint8_t(fleet_activity::unspecified));
				} else if(owner.get_is_at_war()) {
					if(!navy_needs_repair(state, n)) {
						bool valid_attacker = true;
						auto self_ships = state.world.navy_get_navy_membership(n);
						int32_t self_sz = int32_t(self_ships.end() - self_ships.begin());
						for(auto o : owner.get_navy_control()) {
							if(o.get_navy() != n) {
								if(o.get_navy().get_ai_activity() == uint8_t(fleet_activity::attacking)) {
									valid_attacker = false;
									break;
								}
								auto orange = o.get_navy().get_navy_membership();
								if(int32_t(orange.end() - orange.begin()) >= self_sz) {
									valid_attacker = false;
									break;
								}
							}
						}
						if(valid_attacker && naval_advantage(state, owner)) {
							set_fleet_target(state, owner, state.world.navy_get_location_from_navy_location(n), n);
						}
					}
				}
				break;
			case fleet_activity::unloading:
			{
				bool failed_transport = true;

				auto transporting = state.world.navy_get_army_transport(n);
				for(auto ar : transporting) {
					if(ar.get_army().get_path().size() != 0) {
						failed_transport = false;
					}
				}
				if(transporting.begin() == transporting.end()) {
					// all unloaded -> set to unspecified to send home later in this routine
					state.world.navy_set_ai_activity(n, uint8_t(fleet_activity::unspecified));
				} else if(failed_transport) {
					// an army is stuck on the boats
					state.world.navy_set_ai_activity(n, uint8_t(fleet_activity::failed_transport));
				} else {
					// do nothing, still unloading
				}
			}
			break;
			}
		}
	}

	/*  Obtains the supply usage % of a given province.
		@return The supply usage, from 0 to 1 */
	float province_supply_usage(sys::state& state, dcon::nation_id n, dcon::province_id p) {
		auto const attrition_mod = 1.0f + state.world.nation_get_modifier_values(n, sys::national_mod_offsets::land_attrition);
		auto const cur_weight = military::local_army_weight(state, p) * attrition_mod;
		auto const max_weight = military::max_supply_weight_in_province(state, n, p);
		return max_weight > 0.f ? cur_weight / max_weight : 0.f;
	}

	struct classified_province {
		dcon::province_id id;
		float weight;
	};

	void distribute_guards_add_province(sys::state& state, dcon::nation_id n, dcon::province_id p, std::vector<classified_province>& provinces) {
		enum class province_class : uint8_t {
			interior = 0,
			low_priority_border = 1,
			border = 2,
			threat_border = 10,
			hostile_border = 100
		} cls = province_class::interior;
		for(auto padj : state.world.province_get_province_adjacency(p)) {
			auto other = padj.get_connected_provinces(0) == p ? padj.get_connected_provinces(1) : padj.get_connected_provinces(0);
			auto n_controller = other.get_nation_from_province_control();
			auto ovr = n_controller.get_overlord_as_subject().get_ruler();
			if(n_controller == n) {
				// own province
				auto lb = other.get_land_battle_location();
				if(lb.begin() != lb.end() || other.get_siege_progress() > 0.f) {
					cls = province_class::hostile_border;
					break;
				}
				if(other == state.world.nation_get_capital(n)) {
					cls = province_class::border;
					break;
				}
			} else if(other.get_rebel_faction_from_province_rebel_control()) {
				cls = province_class::hostile_border;
				break;
			} else if(n_controller) {
				/*	We will target POTENTIAL enemies of the nation; we could also check if
					the CB can be used on us, but that is expensive, so instead we use available_cbs! */
				if(n_controller && state.world.nation_get_ai_rival(n) == n_controller) {
					cls = province_class::hostile_border;
					break;
				} else if(military::are_at_war(state, n, n_controller)) {
					cls = province_class::hostile_border;
					break;
				} else if(nations::are_allied(state, n, n_controller) || (ovr && ovr == n) || (ovr && nations::are_allied(state, n, ovr))) {
					// allied controller or subject of allied controller or our "parent" overlord
					if(uint8_t(cls) < uint8_t(province_class::low_priority_border)) {
						cls = province_class::low_priority_border;
					}
				} else {
					bool is_threat = false;
					is_threat |= n_controller.get_ai_rival() == n;
					is_threat |= state.world.nation_get_ai_rival(n) == n_controller.id;
					if(ovr) {
						/* subjects cannot negotiate by themselves, but the overlord may */
						is_threat |= ovr.get_ai_rival() == n;
						is_threat |= state.world.nation_get_ai_rival(n) == ovr.id;
						is_threat |= ovr.get_constructing_cb_target() == n;
						for(auto cb : ovr.get_available_cbs()) {
							is_threat |= cb.target == n;
						}
					} else {
						is_threat |= n_controller.get_constructing_cb_target() == n;
						for(auto cb : n_controller.get_available_cbs()) {
							is_threat |= cb.target == n;
						}
					}
					if(is_threat) {
						if(uint8_t(cls) < uint8_t(province_class::threat_border)) {
							cls = province_class::threat_border;
						}
					} else { // other border
						if(uint8_t(cls) < uint8_t(province_class::border)) {
							cls = province_class::border;
						}
					}
				}
			}
		}

		auto w = (float(cls) * 5.f) * ai::province_strategic_weight<float>(state, p);
		w *= 1.5f - ai::province_supply_usage(state, n, p);
		provinces.push_back(classified_province{ p, w });
	}

	void distribute_guards(sys::state& state, dcon::nation_id n) {
		std::vector<classified_province> provinces;
		provinces.reserve(state.world.province_size());
		for(auto c : state.world.nation_get_province_control(n)) { //ourselves
			distribute_guards_add_province(state, n, c.get_province(), provinces);
		}
		for(auto ov : state.world.nation_get_overlord_as_ruler(n)) { //puppets
			for(auto const c : ov.get_subject().get_province_control()) {
				distribute_guards_add_province(state, n, c.get_province(), provinces);
			}
		}
		pdqsort(provinces.begin(), provinces.end(), [&](classified_province& a, classified_province& b) {
			if(a.weight != b.weight)
				return a.weight > b.weight;
			return a.id.index() < b.id.index();
		});

		// form list of guards
		std::vector<dcon::army_id> guards_list;
		guards_list.reserve(state.world.army_size());
		for(auto a : state.world.nation_get_army_control(n)) {
			if(a.get_army().get_ai_activity() == uint8_t(army_activity::on_guard)) {
				guards_list.push_back(a.get_army().id);
			}
		}

		// distribute target provinces
		bool guard_assigned = false;
		do {
			guard_assigned = false;
			for(uint32_t i = 0; i < provinces.size() && !guards_list.empty(); ++i) {
				auto p = provinces[i].id;
				auto p_region = state.world.province_get_connected_region_id(provinces[i].id);
				assert(p_region > 0);
				uint32_t nearest_index = 0;
				dcon::army_id nearest;
				float nearest_distance = 0.f;
				for(uint32_t k = uint32_t(guards_list.size()); k-- > 0;) {
					auto const guard_loc = state.world.army_get_location_from_army_location(guards_list[k]);
					auto const d = province::sorting_distance(state, guard_loc, p);
					/* Must not be starving, otherwise no point in gathering */
					auto const avg_org = ai::average_army_org(state, guards_list[k]);
					auto const avg_str = ai::average_army_strength(state, guards_list[k]);
					if(d < nearest_distance && (avg_org > 0.5f && avg_str > 0.5f)) {
						nearest_index = k;
						nearest_distance = d;
						nearest = guards_list[k];
					}
				}
				// assign nearest guard
				if(nearest) {
					state.world.army_set_ai_province(nearest, p);
					guards_list[nearest_index] = guards_list.back();
					guards_list.pop_back();
					guard_assigned = true;
				}
			}
		} while(guard_assigned);
	}

	dcon::navy_id find_transport_fleet(sys::state& state, dcon::nation_id controller) {
		int32_t n_size = 0;
		dcon::navy_id transport_fleet;

		for(auto v : state.world.nation_get_navy_control(controller)) {
			if(v.get_navy().get_battle_from_navy_battle_participation())
				continue;
			auto members = v.get_navy().get_navy_membership();

			auto tsize = int32_t(members.end() - members.begin());
			if(tsize <= n_size || tsize <= 1) {
				continue;
			}

			n_size = tsize;
			transport_fleet = dcon::navy_id{};
			fleet_activity activity = fleet_activity(v.get_navy().get_ai_activity());
			if(activity == fleet_activity::attacking || activity == fleet_activity::idle || activity == fleet_activity::returning_to_base) {
				auto in_transport = v.get_navy().get_army_transport();
				if(in_transport.begin() == in_transport.end()) {
					transport_fleet = v.get_navy();
				}
			}
		}
		return transport_fleet;
	}

	void move_idle_guards(sys::state& state) {
		std::vector<dcon::army_id> require_transport;
		require_transport.reserve(state.world.army_size());

		for(auto ar : state.world.in_army) {
			if(ar.get_ai_activity() == uint8_t(army_activity::on_guard)
			&& ar.get_ai_province()
			&& ar.get_ai_province() != ar.get_location_from_army_location()
			&& ar.get_controller_from_army_control()
			&& unit_on_ai_control(state, ar)
			&& !ar.get_arrival_time()
			&& !ar.get_battle_from_army_battle_participation()
			&& !ar.get_navy_from_army_transport()) {
				auto path = ar.get_black_flag() ? province::make_unowned_land_path(state, ar.get_location_from_army_location(), ar.get_ai_province()) : province::make_land_path(state, ar.get_location_from_army_location(), ar.get_ai_province(), ar.get_controller_from_army_control(), ar);
				if(path.size() > 0) {
					auto existing_path = ar.get_path();
					auto new_size = uint32_t(path.size());
					existing_path.resize(new_size);

					for(uint32_t i = 0; i < new_size; ++i) {
						assert(path[i]);
						existing_path[i] = path[i];
					}
					ar.set_arrival_time(military::arrival_time_to(state, ar, path.back()));
					ar.set_dig_in(0);
				} else if(!ar.get_controller_from_army_control().get_is_player_controlled()) {
					//Units delegated to the AI won't transport themselves on their own
					require_transport.push_back(ar.id);
				}
			}
		}

		for(uint32_t i = 0; i < require_transport.size(); ++i) {
			auto coastal_target_prov = state.world.army_get_location_from_army_location(require_transport[i]);
			auto controller = state.world.army_get_controller_from_army_control(require_transport[i]);

			dcon::navy_id transport_fleet = find_transport_fleet(state, controller);

			auto regs = state.world.army_get_army_membership(require_transport[i]);

			auto tcap = military::transport_capacity(state, transport_fleet);
			tcap -= int32_t(regs.end() - regs.begin());

			if(tcap < 0 || (state.world.nation_get_is_at_war(controller) && !naval_advantage(state, controller))) {
				for(uint32_t j = uint32_t(require_transport.size()); j-- > i + 1;) {
					if(state.world.army_get_controller_from_army_control(require_transport[j]) == controller) {
					state.world.army_set_ai_province(require_transport[j], dcon::province_id{}); // stop rechecking these units
						require_transport[j] = require_transport.back();
						require_transport.pop_back();
					}
				}
				state.world.army_set_ai_province(require_transport[i], dcon::province_id{}); // stop rechecking unit
				continue;
			}

			if(!state.world.province_get_is_coast(coastal_target_prov)) {
				auto path = state.world.army_get_black_flag(require_transport[i])
				? province::make_unowned_path_to_nearest_coast(state, coastal_target_prov)
				: province::make_path_to_nearest_coast(state, controller, coastal_target_prov);
				if(path.empty()) {
				state.world.army_set_ai_province(require_transport[i], dcon::province_id{}); // stop rechecking unit
					continue; // army could not reach coast
				} else {
					coastal_target_prov = path.front();

					auto existing_path = state.world.army_get_path(require_transport[i]);
					auto new_size = uint32_t(path.size());
					existing_path.resize(new_size);

					for(uint32_t k = 0; k < new_size; ++k) {
						assert(path[k]);
						existing_path[k] = path[k];
					}
					state.world.army_set_arrival_time(require_transport[i], military::arrival_time_to(state, require_transport[i], path.back()));
					state.world.army_set_dig_in(require_transport[i], 0);
				}
			}
			{
				auto fleet_destination = province::has_naval_access_to_province(state, controller, coastal_target_prov) ? coastal_target_prov : state.world.province_get_port_to(coastal_target_prov);
				if(fleet_destination == state.world.navy_get_location_from_navy_location(transport_fleet)) {
					state.world.navy_get_path(transport_fleet).clear();
					state.world.navy_set_arrival_time(transport_fleet, sys::date{});
					state.world.navy_set_ai_activity(transport_fleet, uint8_t(fleet_activity::boarding));
				} else if(auto fleet_path = province::make_naval_path(state, state.world.navy_get_location_from_navy_location(transport_fleet), fleet_destination); fleet_path.empty()) { // this essentially should be impossible ...
					continue;
				} else {
					auto existing_path = state.world.navy_get_path(transport_fleet);
					auto new_size = uint32_t(fleet_path.size());
					existing_path.resize(new_size);

					for(uint32_t k = 0; k < new_size; ++k) {
						assert(fleet_path[k]);
						existing_path[k] = fleet_path[k];
					}
					state.world.navy_set_arrival_time(transport_fleet, military::arrival_time_to(state, transport_fleet, fleet_path.back()));
					state.world.navy_set_ai_activity(transport_fleet, uint8_t(fleet_activity::boarding));
				}
			}

			state.world.army_set_ai_activity(require_transport[i], uint8_t(army_activity::transport_guard));

			auto destination_region = state.world.province_get_connected_region_id(state.world.army_get_ai_province(require_transport[i]));

			// scoop up other armies to transport
			for(uint32_t j = uint32_t(require_transport.size()); j-- > i + 1;) {
				if(state.world.army_get_controller_from_army_control(require_transport[j]) == controller) {
					auto jregs = state.world.army_get_army_membership(require_transport[j]);
					if(tcap >= (jregs.end() - jregs.begin())) { // check if it will fit
						if(state.world.province_get_connected_region_id(state.world.army_get_ai_province(require_transport[j])) != destination_region)
						continue;

						if(state.world.army_get_location_from_army_location(require_transport[j]) == coastal_target_prov) {
							state.world.army_set_ai_activity(require_transport[i], uint8_t(army_activity::transport_guard));
							tcap -= int32_t(jregs.end() - jregs.begin());
						} else {
							auto jpath = state.world.army_get_black_flag(require_transport[j])
							? province::make_land_path(state, state.world.army_get_location_from_army_location(require_transport[j]), coastal_target_prov, controller, require_transport[j])
							: province::make_unowned_land_path(state, state.world.army_get_location_from_army_location(require_transport[j]), coastal_target_prov);
							if(!jpath.empty()) {
								auto existing_path = state.world.army_get_path(require_transport[j]);
								auto new_size = uint32_t(jpath.size());
								existing_path.resize(new_size);

								for(uint32_t k = 0; k < new_size; ++k) {
									assert(jpath[k]);
									existing_path[k] = jpath[k];
								}
								state.world.army_set_arrival_time(require_transport[j], military::arrival_time_to(state, require_transport[j], jpath.back()));
								state.world.army_set_dig_in(require_transport[j], 0);
								state.world.army_set_ai_activity(require_transport[i], uint8_t(army_activity::transport_guard));
								tcap -= int32_t(jregs.end() - jregs.begin());
							}
						}
					}

					require_transport[j] = require_transport.back();
					require_transport.pop_back();
				}
			}
		}
	}

	void update_naval_transport(sys::state& state) {

		// set armies to move into transports
		for(auto ar : state.world.in_army) {
			if(ar.get_battle_from_army_battle_participation())
			continue;
			if(ar.get_navy_from_army_transport())
			continue;
			if(ar.get_arrival_time())
			continue;

			if(ar.get_ai_activity() == uint8_t(army_activity::transport_guard) || ar.get_ai_activity() == uint8_t(army_activity::transport_attack)) {
				auto controller = ar.get_controller_from_army_control();
				dcon::navy_id transports;
				for(auto v : controller.get_navy_control()) {
					if(v.get_navy().get_ai_activity() == uint8_t(fleet_activity::boarding)) {
						transports = v.get_navy();
					}
				}
				if(!transports) {
					ar.set_ai_activity(uint8_t(army_activity::on_guard));
				ar.set_ai_province(dcon::province_id{});
					continue;
				}
				if(state.world.navy_get_arrival_time(transports) || state.world.navy_get_battle_from_navy_battle_participation(transports))
				continue; // still moving

				auto army_location = ar.get_location_from_army_location();
				auto transport_location = state.world.navy_get_location_from_navy_location(transports);
				if(transport_location == army_location) {
					ar.set_navy_from_army_transport(transports);
					ar.set_black_flag(false);
				} else if(army_location.get_port_to() == transport_location) {
					auto existing_path = ar.get_path();
					existing_path.resize(1);
					assert(transport_location);
					existing_path[0] = transport_location;
					ar.set_arrival_time(military::arrival_time_to(state, ar, transport_location));
					ar.set_dig_in(0);
				} else { // transport arrived in inaccessible location
					ar.set_ai_activity(uint8_t(army_activity::on_guard));
				ar.set_ai_province(dcon::province_id{});
				}
			}
		}
	}

	bool army_ready_for_battle(sys::state& state, dcon::nation_id n, dcon::army_id a) {
		dcon::regiment_id sample_reg;
		auto regs = state.world.army_get_army_membership(a);
		if(regs.begin() != regs.end()) {
			sample_reg = (*regs.begin()).get_regiment().id;
		} else {
			return false;
		}


		auto spending_level = state.world.nation_get_effective_land_spending(n);
		auto max_org = 0.25f + 0.75f * spending_level;

		return state.world.regiment_get_org(sample_reg) > 0.7f * max_org;
	}

	/* DEPRECATED */
	void gather_to_battle(sys::state& state, dcon::nation_id n, dcon::province_id p) {
		for(auto ar : state.world.nation_get_army_control(n)) {
			army_activity activity = army_activity(ar.get_army().get_ai_activity());
			if(ar.get_army().get_battle_from_army_battle_participation()
			|| ar.get_army().get_navy_from_army_transport()
			|| ar.get_army().get_black_flag()
			|| ar.get_army().get_arrival_time()
			|| (activity != army_activity::on_guard && activity != army_activity::attacking && activity != army_activity::attack_gathered && activity != army_activity::attack_transport)
			|| !army_ready_for_battle(state, n, ar.get_army())) {
				continue;
			}

			auto location = ar.get_army().get_location_from_army_location();
			if(location == p) {
				continue;
			}

			auto sdist = province::sorting_distance(state, location, p);
			if(sdist > state.defines.alice_ai_gather_radius) {
				continue;
			}

			auto jpath = province::make_land_path(state, location, p, n, ar.get_army());
			if(!jpath.empty()) {
				auto existing_path = ar.get_army().get_path();
				auto new_size = uint32_t(jpath.size());
				existing_path.resize(new_size * 2);

				for(uint32_t k = 0; k < new_size; ++k) {
					assert(jpath[k]);
					existing_path[new_size + k] = jpath[k];
				}
				for(uint32_t k = 1; k < new_size; ++k) {
					assert(jpath[k]);
					existing_path[new_size - k] = jpath[k];
				}
				assert(location);
				existing_path[0] = location;
				ar.get_army().set_arrival_time(military::arrival_time_to(state, ar.get_army(), jpath.back()));
				ar.get_army().set_dig_in(0);
			}
		}
	}

	float estimate_unit_type_value(sys::state& state, dcon::nation_id n, dcon::unit_type_id utid) {
		auto const& ut = state.military_definitions.unit_base_definitions[utid];
		auto const& uts = state.world.nation_get_unit_stats(n, utid);
		switch(state.military_definitions.unit_base_definitions[utid].type) {
		case military::unit_type::cavalry:
		case military::unit_type::infantry:
		{
			float atk = (uts.attack_or_gun_power + 1.f) / uts.discipline_or_evasion;
			float def = (uts.attack_or_gun_power + 1.f) / uts.discipline_or_evasion;
			return std::max(atk * def, 1.f);
		}
		case military::unit_type::support:
		case military::unit_type::special:
		{
			float atk = (uts.attack_or_gun_power * 0.1f + 1.0f) * uts.support / uts.discipline_or_evasion;
			float def = (uts.attack_or_gun_power * 0.1f + 1.0f) * uts.support / uts.discipline_or_evasion;
			return std::max(atk * def, 1.f);
		}
		default:
			break;
		}
		return 0.f;
	}

	void update_frontline_counters(sys::state& state, dcon::unit_type_id utid, int32_t& num_frontline, int32_t& num_support, int32_t& num_cavalry) {
		if(state.military_definitions.unit_base_definitions[utid].type == military::unit_type::support
		|| state.military_definitions.unit_base_definitions[utid].type == military::unit_type::special) {
			++num_support;
			return;
		} else if(state.military_definitions.unit_base_definitions[utid].type == military::unit_type::cavalry) {
			++num_cavalry;
			return;
		}
		++num_frontline;
	}

	float estimate_balanced_composition_factor(sys::state& state, dcon::army_id a) {
		auto regs = state.world.army_get_army_membership(a);
		// account composition
		// Ideal composition: 4/1/4 (1 cavalry for each 4 infantry and 1 infantry for each arty)
		float total_str = 0.f;
		float str_art = 0.f;
		float str_inf = 0.f;
		float str_cav = 0.f;
		for(const auto reg : regs) {
			float str = reg.get_regiment().get_strength() * reg.get_regiment().get_org();
			if(auto utid = reg.get_regiment().get_type(); utid) {
				switch(state.military_definitions.unit_base_definitions[utid].type) {
				case military::unit_type::infantry:
					str_inf += str;
					break;
				case military::unit_type::cavalry:
					str_cav += str;
					break;
				case military::unit_type::support:
				case military::unit_type::special:
					str_art += str;
					break;
				default:
					break;
				}
			}
			total_str += str;
		}
		if(total_str == 0.f) {
			return 0.f;
		}
		if((str_cav + str_inf) == str_art) {
			return 1.f;
		}
		return (str_cav + str_inf) > 0.f ? std::max(str_art / (str_cav + str_inf), 0.01f) : 0.01f;
	}

	float estimate_army_defensive_strength(sys::state& state, dcon::army_id a) {
		auto scale = state.world.army_get_controller_from_army_control(a) ? 1.f : 0.5f;
		// account general
		auto n = state.world.army_get_controller_from_army_control(a);
		if(!n) {
			n = state.world.national_identity_get_nation_from_identity_holder(state.national_definitions.rebel_id);
		}
		if(auto gen = state.world.army_get_general_from_army_leadership(a); gen) {
			auto bg = state.world.leader_get_background(gen);
			bg = bool(bg) ? bg : state.military_definitions.no_background;
			auto per = state.world.leader_get_personality(gen);
			per = bool(per) ? per : state.military_definitions.no_personality;
			auto morale = state.world.nation_get_modifier_values(n, sys::national_mod_offsets::org_regain)
				+ state.world.leader_trait_get_morale(bg)
				+ state.world.leader_trait_get_morale(per) + 1.0f;
			auto org = state.world.nation_get_modifier_values(n, sys::national_mod_offsets::land_organisation)
				+ state.world.leader_trait_get_organisation(bg)
				+ state.world.leader_trait_get_organisation(per) + 1.0f;
			auto def = state.world.nation_get_modifier_values(n, sys::national_mod_offsets::land_attack_modifier)
				+ state.world.leader_trait_get_defense(bg)
				+ state.world.leader_trait_get_defense(per) + 1.0f;
			scale += def * (morale * org * 0.5f);
		}
		scale += state.world.province_get_modifier_values(state.world.army_get_location_from_army_location(a), sys::provincial_mod_offsets::defense);
		scale += 1.0f + 0.1f * state.world.province_get_building_level(state.world.army_get_location_from_army_location(a), state.economy_definitions.fort_building);
		// composition bonus
		scale += state.world.nation_get_has_gas_defense(n) ? state.defines.gas_attack_modifier : 0.f;
		auto const strength = std::max(0.125f, estimate_balanced_composition_factor(state, a));
		return std::max(0.1f, strength * scale);
	}

	float estimate_army_offensive_strength(sys::state& state, dcon::army_id a) {
		auto scale = state.world.army_get_controller_from_army_control(a) ? 1.f : 0.5f;
		// account general
		auto n = state.world.army_get_controller_from_army_control(a);
		if(!n) {
			n = state.world.national_identity_get_nation_from_identity_holder(state.national_definitions.rebel_id);
		}
		if(auto gen = state.world.army_get_general_from_army_leadership(a); gen) {
			auto bg = state.world.leader_get_background(gen);
			bg = bool(bg) ? bg : state.military_definitions.no_background;
			auto per = state.world.leader_get_personality(gen);
			per = bool(per) ? per : state.military_definitions.no_personality;
			auto morale = state.world.nation_get_modifier_values(n, sys::national_mod_offsets::org_regain)
				+ state.world.leader_trait_get_morale(bg)
				+ state.world.leader_trait_get_morale(per) + 1.0f;
			auto org = state.world.nation_get_modifier_values(n, sys::national_mod_offsets::land_organisation)
				+ state.world.leader_trait_get_organisation(bg)
				+ state.world.leader_trait_get_organisation(per) + 1.0f;
			auto atk = state.world.nation_get_modifier_values(n, sys::national_mod_offsets::land_attack_modifier)
				+ state.world.leader_trait_get_attack(bg)
				+ state.world.leader_trait_get_attack(per) + 1.0f;
			scale += atk * (morale * org * 0.5f);
		}
		// composition bonus
		scale += state.world.nation_get_has_gas_attack(n) ? state.defines.gas_attack_modifier : 0.f;
		auto const strength = std::max(0.125f, estimate_balanced_composition_factor(state, a));
		return std::max(0.1f, strength * scale);
	}

	float estimate_friendly_offensive_force(sys::state& state, dcon::province_id target, dcon::nation_id by) {
		auto total = 0.f;
		for(auto ar : state.world.province_get_army_location(target)) {
			auto other_nation = ar.get_army().get_controller_from_army_control();
			if(other_nation == by || military::are_allied_in_war(state, other_nation, by)) {
				total += estimate_army_offensive_strength(state, ar.get_army());
			}
		}
		return total;
	}

	float estimate_enemy_defensive_force(sys::state& state, dcon::province_id target, dcon::nation_id by) {
		auto total = 0.f;
		for(auto ar : state.world.province_get_army_location(target)) {
			auto other_nation = ar.get_army().get_controller_from_army_control();
			if(!other_nation || military::are_at_war(state, other_nation, by)) {
				total += estimate_army_defensive_strength(state, ar.get_army());
			}
		}
		return total;
	}

	void assign_targets(sys::state& state, dcon::nation_id n) {
		struct a_str {
			dcon::province_id p;
			float str = 0.0f;
		};
		std::vector<a_str> ready_armies;
		ready_armies.reserve(state.world.province_size());

		int32_t ready_count = 0;
		for(auto ar : state.world.nation_get_army_control(n)) {
			army_activity activity = army_activity(ar.get_army().get_ai_activity());
			if(ar.get_army().get_battle_from_army_battle_participation()
			|| ar.get_army().get_navy_from_army_transport()
			|| ar.get_army().get_black_flag()
			|| ar.get_army().get_arrival_time()
			|| army_activity(ar.get_army().get_ai_activity()) != army_activity::on_guard
			|| !army_ready_for_battle(state, n, ar.get_army())) {
				continue;
			}

			++ready_count;
			auto loc = ar.get_army().get_location_from_army_location().id;
			if(std::find_if(ready_armies.begin(), ready_armies.end(), [loc](a_str const& v) { return loc == v.p; }) == ready_armies.end()) {
				ready_armies.push_back(a_str{ loc, 0.0f });
			}
		}

		if(ready_armies.empty())
			return; // nothing to attack with

		struct army_target {
			float minimal_distance;
			dcon::province_id location;
			float strength_estimate = 0.0f;
		};

		/* Ourselves */
		std::vector<army_target> potential_targets;
		potential_targets.reserve(state.world.province_size());
		for(auto o : state.world.nation_get_province_ownership(n)) {
			if(!o.get_province().get_nation_from_province_control() || military::rebel_army_in_province(state, o.get_province())) {
				potential_targets.push_back(
					army_target{ province::sorting_distance(state, o.get_province(), ready_armies[0].p), o.get_province().id, 0.0f }
				);
			}
		}
		/* Nations we're at war with OR hostile to */
		std::vector<dcon::nation_id> at_war_with;
		at_war_with.reserve(state.world.nation_size());
		for(auto w : state.world.nation_get_war_participant(n)) {
			auto attacker = w.get_is_attacker();
			for(auto p : w.get_war().get_war_participant()) {
				if(p.get_is_attacker() != attacker) {
					if(std::find(at_war_with.begin(), at_war_with.end(), p.get_nation().id) == at_war_with.end()) {
						at_war_with.push_back(p.get_nation().id);
					}
				}
			}
		}
		for(auto w : at_war_with) {
			for(auto o : state.world.nation_get_province_control(w)) {
				potential_targets.push_back(
					army_target{ province::sorting_distance(state, o.get_province(), ready_armies[0].p), o.get_province().id, 0.0f }
				);
			}
			for(auto o : state.world.nation_get_province_ownership(w)) {
				if(!o.get_province().get_nation_from_province_control()) {
					potential_targets.push_back(
						army_target{ province::sorting_distance(state, o.get_province(), ready_armies[0].p), o.get_province().id, 0.0f }
					);
				}
			}
		}
		/* Our allies (mainly our substates, vassals) - we need to care of them! */
		for(const auto ovr : state.world.nation_get_overlord_as_ruler(n)) {
			auto w = ovr.get_subject();
			for(auto o : state.world.nation_get_province_ownership(w)) {
				if(!o.get_province().get_nation_from_province_control() || military::rebel_army_in_province(state, o.get_province())) {
					potential_targets.push_back(
						army_target{ province::sorting_distance(state, o.get_province(), ready_armies[0].p), o.get_province().id, 0.0f }
					);
				}
			}
		}

		for(auto& pt : potential_targets) {
			for(uint32_t i = uint32_t(ready_armies.size()); i-- > 1;) {
				auto sdist = province::sorting_distance(state, ready_armies[i].p, pt.location);
				if(sdist < pt.minimal_distance) {
					pt.minimal_distance = sdist;
				}
			}
		}
		pdqsort(potential_targets.begin(), potential_targets.end(), [&](army_target& a, army_target& b) {
			if(a.minimal_distance != b.minimal_distance)
				return a.minimal_distance < b.minimal_distance;
			return a.location.index() < b.location.index();
		});

		// organize attack stacks
		bool is_at_war = state.world.nation_get_is_at_war(n);
		int32_t min_ready_count = std::min(ready_count, 3); //Atleast 3 attacks
		int32_t max_attacks_to_make = is_at_war ? std::max(min_ready_count, (ready_count + 1) / 3) : ready_count; // not at war -- allow all stacks to attack rebels
		auto const psize = potential_targets.size();

		for(uint32_t i = 0; i < psize && max_attacks_to_make > 0; ++i) {
			if(!potential_targets[i].location) {
				continue; // target has been removed as too close by some earlier iteration
			}
			if(potential_targets[i].strength_estimate == 0.0f) {
				auto const friend_str = estimate_friendly_offensive_force(state, potential_targets[i].location, n);
				auto const enemy_str = estimate_enemy_defensive_force(state, potential_targets[i].location, n);
				potential_targets[i].strength_estimate = enemy_str - friend_str;
				potential_targets[i].strength_estimate = std::max(0.001f, potential_targets[i].strength_estimate);
			}

			auto target_attack_force = potential_targets[i].strength_estimate;
			pdqsort(ready_armies.begin(), ready_armies.end(), [&](a_str const& a, a_str const& b) {
				auto adist = province::sorting_distance(state, a.p, potential_targets[i].location);
				auto bdist = province::sorting_distance(state, b.p, potential_targets[i].location);
				if(adist != bdist)
					return adist > bdist;
				return a.p.index() < b.p.index();
			});

			// make list of attackers
			float a_force_str = 0.f;
			int32_t k = int32_t(ready_armies.size());
			for(; k-- > 0 && a_force_str <= target_attack_force;) {
				if(ready_armies[k].str == 0.0f) {
					for(auto ar : state.world.province_get_army_location(ready_armies[k].p)) {
						if(ar.get_army().get_battle_from_army_battle_participation()
						|| n != ar.get_army().get_controller_from_army_control()
						|| ar.get_army().get_navy_from_army_transport()
						|| ar.get_army().get_black_flag()
						|| ar.get_army().get_arrival_time()
						|| army_activity(ar.get_army().get_ai_activity()) != army_activity::on_guard
						|| !army_ready_for_battle(state, n, ar.get_army())) {
							continue;
						}
						ready_armies[k].str += estimate_army_offensive_strength(state, ar.get_army());
					}
					ready_armies[k].str = std::max(0.001f, ready_armies[k].str);
				}
				a_force_str += ready_armies[k].str;
			}

			if(a_force_str < target_attack_force) {
				return; // end assigning attackers completely
			}

			// find central province
			dcon::province_id central_province;

			glm::vec3 accumulated{ 0.0f, 0.0f, 0.0f };
			float minimal_distance = 2.0f;

			for(int32_t m = int32_t(ready_armies.size()); m-- > k + 1; ) {
				accumulated += state.world.province_get_mid_point_b(ready_armies[m].p);
			}
			auto magnitude = math::sqrt((accumulated.x * accumulated.x + accumulated.y * accumulated.y) + accumulated.z * accumulated.z);
			accumulated /= std::max(magnitude, 0.001f);

			province::for_each_land_province(state, [&](dcon::province_id p) {
				if(!province::has_safe_access_to_province(state, n, p)) {
					return;
				}
				auto pmid = state.world.province_get_mid_point_b(p);
				if(auto dist = -((accumulated.x * pmid.x + accumulated.y * pmid.y) + accumulated.z * pmid.z); dist < minimal_distance) {
					minimal_distance = dist;
					central_province = p;
				}
			});
			if(!central_province) {
				continue;
			}

			// issue safe-move gather command
			for(int32_t m = int32_t(ready_armies.size()); m-- > k + 1; ) {
				assert(m >= 0 && m < int32_t(ready_armies.size()));
				for(auto ar : state.world.province_get_army_location(ready_armies[m].p)) {
					if(ar.get_army().get_battle_from_army_battle_participation()
					|| n != ar.get_army().get_controller_from_army_control()
					|| ar.get_army().get_navy_from_army_transport()
					|| ar.get_army().get_black_flag()
					|| ar.get_army().get_arrival_time()
					|| army_activity(ar.get_army().get_ai_activity()) != army_activity::on_guard
					|| !army_ready_for_battle(state, n, ar.get_army())) {
						continue;
					}

					if(ready_armies[m].p == central_province) {
						ar.get_army().set_ai_province(potential_targets[i].location);
						ar.get_army().set_ai_activity(uint8_t(army_activity::attacking));
					} else if(auto path = province::make_safe_land_path(state, ready_armies[m].p, central_province, n); !path.empty()) {
						auto existing_path = ar.get_army().get_path();
						auto new_size = uint32_t(path.size());
						existing_path.resize(new_size);
						for(uint32_t q = 0; q < new_size; ++q) {
							assert(path[q]);
							existing_path[q] = path[q];
						}
						ar.get_army().set_arrival_time(military::arrival_time_to(state, ar.get_army(), path.back()));
						ar.get_army().set_dig_in(0);
						ar.get_army().set_ai_province(potential_targets[i].location);
						ar.get_army().set_ai_activity(uint8_t(army_activity::attacking));
					}
				}
			}

			ready_armies.resize(k + 1);
			--max_attacks_to_make;

			// remove subsequent targets that are too close
			if(is_at_war) {
				for(uint32_t j = i + 1; j < psize; ++j) {
					if(province::sorting_distance(state, potential_targets[j].location, potential_targets[i].location) < state.defines.alice_ai_attack_target_radius) {
						potential_targets[j].location = dcon::province_id{};
					}
				}
			}
		}
	}

	/*bool retreating_would_end_battle(sys::state& state, dcon::land_battle_id lb, bool is_attacker) {
		auto w = state.world.land_battle_get_war_from_land_battle_in_war(lb);
		for(auto const ar : state.world.land_battle_get_army_battle_participation_as_battle(lb)) {
			auto const controller = ar.get_army().get_controller_from_army_control();
			if(is_attacker == (military::get_role(state, w, controller) == military::war_role::attacker)) {
				return false; //an ally is on the battle
			}
		}
		return true;
	}*/

	/*	Obtains the stats of a leader, with respect to the given battle
		For example, an attack +5, defense -5 general would be extremely unhelpful
		in defensive battles. So we return a -5, but it would be extremely helpful in
		a +5 attack battle. */
	float cycling_get_leader_stats_wrt_battle(sys::state& state, dcon::leader_id l, bool is_attacker) {
		auto bg = state.world.leader_get_background(l);
		bg = bool(bg) ? bg : state.military_definitions.no_background;
		auto per = state.world.leader_get_personality(l);
		per = bool(per) ? per : state.military_definitions.no_personality;
		if(is_attacker) {
			return bg.get_attack() + per.get_attack();
		}
		return bg.get_defense() + per.get_defense();
	}

	/* Assign leaders to units without an assigned leader */
	void assign_leaders(sys::state& state) {
		for(const auto ll : state.world.in_leader_loyalty) {
			if(!ll.get_nation() || ll.get_nation().get_is_player_controlled())
				continue;
			if(ll.get_leader().get_is_admiral() && !ll.get_leader().get_navy_from_navy_leadership()) {
				for(const auto nc : ll.get_nation().get_navy_control()) {
					auto const n = nc.get_navy();
					if(!n.get_admiral_from_navy_leadership()
					&& !n.get_battle_from_navy_battle_participation()
					&& !n.get_is_retreating()) {
						n.set_admiral_from_navy_leadership(ll.get_leader());
						break;
					}
				}
			} else if(!ll.get_leader().get_is_admiral() && !ll.get_leader().get_army_from_army_leadership()) {
				for(const auto ac : ll.get_nation().get_army_control()) {
					auto const a = ac.get_army();
					if(!a.get_general_from_army_leadership()
					&& !a.get_battle_from_army_battle_participation()
					&& !a.get_is_retreating()
					&& !a.get_black_flag()) {
						a.set_general_from_army_leadership(ll.get_leader());
						break;
					}
				}
			}
		}
	}

	/* Performs cycling inbetween battles */
	void perform_cycling(sys::state& state) {
		//code in charge of cycling armies
		for(auto lb : state.world.in_land_battle) {
			if(!military::can_retreat_from_battle(state, lb))
				continue; //can't cycle yet
			for(auto ar : state.world.land_battle_get_army_battle_participation(lb)) {
				auto n = ar.get_army().get_controller_from_army_control();
				if(ar.get_battle() && ar.get_army() && (!n || !n.get_is_player_controlled())) {
					auto al = ar.get_battle().get_attacking_general();
					auto dl = ar.get_battle().get_defending_general();
					auto w = ar.get_battle().get_war_from_land_battle_in_war();
					auto role = military::get_role(state, w, n);
					if(role == military::war_role::none) {
						continue;
					}
					auto is_attacker =
						(ar.get_battle().get_war_attacker_is_attacker() && role == military::war_role::attacker)
						|| (!ar.get_battle().get_war_attacker_is_attacker() && role == military::war_role::defender);
					auto const att_stats = cycling_get_leader_stats_wrt_battle(state, al.get_general(), true);
					auto const def_stats = cycling_get_leader_stats_wrt_battle(state, dl.get_general(), false);

					/* Keep in battle if we have a better general */
					bool keep_in_battle = is_attacker ? (att_stats > def_stats) : (def_stats > att_stats);
					auto avg_str = average_army_strength(state, ar.get_army());
					auto avg_org = average_army_org(state, ar.get_army());
					if((avg_str < 0.15f || avg_org < 0.15f) && !keep_in_battle) {
						dcon::province_id best_prov;
						auto const p = ar.get_battle().get_location_from_land_battle_location();
						for(auto const adj : p.get_province_adjacency()) {
							auto const o = adj.get_connected_provinces(p == adj.get_connected_provinces(0) ? 1 : 0);
							if(province::has_access_to_province(state, n, o)) {
								if(!best_prov) {
									best_prov = o;
								}
								if(n == o.get_nation_from_province_control()
								|| military::are_allied_in_war(state, n, o.get_nation_from_province_control())) {
									best_prov = o;
								}
							}
						}
						if(best_prov && command::can_move_army(state, n, ar.get_army(), best_prov).size() > 0) {
							//bool is_attacker = lb.get_war_attacker_is_attacker();
							//auto role = military::get_role(state, lb.get_war_from_land_battle_in_war(), ar.get_army().get_controller_from_army_control());
							//if(retreating_would_end_battle(state, lb, is_attacker == (role == military::war_role::attacker))) {
							command::move_army(state, n, ar.get_army(), best_prov, true);
						}
					}
				}
			}
		}
	}

	void make_attacks(sys::state& state) {
		assign_leaders(state);
		perform_cycling(state);
		concurrency::parallel_for(uint32_t(0), state.world.nation_size(), [&](uint32_t i) {
			dcon::nation_id n{ dcon::nation_id::value_base_t(i) };
			if(state.world.nation_is_valid(n)) {
				assign_targets(state, n);
			}
		});
	}

	void make_defense(sys::state& state) {
		concurrency::parallel_for(uint32_t(0), state.world.nation_size(), [&](uint32_t i) {
			dcon::nation_id n{ dcon::nation_id::value_base_t(i) };
			if(state.world.nation_is_valid(n)) {
				distribute_guards(state, n);
			}
		});
	}

	void move_gathered_attackers(sys::state& state) {
		static std::vector<dcon::army_id> require_transport;
		require_transport.clear();

		for(auto ar : state.world.in_army) {
			if(ar.get_ai_activity() == uint8_t(army_activity::attack_transport)) {
				if(!ar.get_arrival_time()
				&& !ar.get_battle_from_army_battle_participation()
				&& !ar.get_navy_from_army_transport()
				&& std::find(require_transport.begin(), require_transport.end(), ar.id) == require_transport.end()) {
					// try to transport
					if(province::has_access_to_province(state, ar.get_controller_from_army_control(), ar.get_ai_province())) {
						require_transport.push_back(ar.id);
					} else {
						ar.set_ai_activity(uint8_t(army_activity::on_guard));
						ar.set_ai_province(dcon::province_id{});
					}
				}
			} else if(ar.get_ai_activity() == uint8_t(army_activity::attack_gathered)) {
				if(!ar.get_arrival_time()
				&& !ar.get_battle_from_army_battle_participation()
				&& !ar.get_navy_from_army_transport()) {

					if(ar.get_location_from_army_location() == ar.get_ai_province()) { // attack finished ?
						if(ar.get_location_from_army_location().get_nation_from_province_control() && !military::are_at_war(state, ar.get_location_from_army_location().get_nation_from_province_control(), ar.get_controller_from_army_control())) {
							ar.set_ai_activity(uint8_t(army_activity::on_guard));
							ar.set_ai_province(dcon::province_id{});
						}
					} else {
						if(province::has_access_to_province(state, ar.get_controller_from_army_control(), ar.get_ai_province())) {
							if(auto path = province::make_land_path(state, ar.get_location_from_army_location(), ar.get_ai_province(), ar.get_controller_from_army_control(), ar); path.size() > 0) {
								auto existing_path = ar.get_path();
								auto new_size = uint32_t(path.size());
								existing_path.resize(new_size);
								for(uint32_t i = 0; i < new_size; ++i) {
									assert(path[i]);
									existing_path[i] = path[i];
								}
								ar.set_arrival_time(military::arrival_time_to(state, ar, path.back()));
								ar.set_dig_in(0);
							} else {
								ar.set_ai_activity(uint8_t(army_activity::on_guard));
								ar.set_ai_province(dcon::province_id{});
							}
						} else {
							ar.set_ai_activity(uint8_t(army_activity::on_guard));
							ar.set_ai_province(dcon::province_id{});
						}
					}
				}
			} else if(ar.get_ai_activity() == uint8_t(army_activity::attacking)
				&& ar.get_ai_province() != ar.get_location_from_army_location()
				&& !ar.get_arrival_time()
				&& !ar.get_battle_from_army_battle_participation()
				&& !ar.get_navy_from_army_transport()) {
				bool all_gathered = true;
				for(auto o : ar.get_controller_from_army_control().get_army_control()) {
					if(o.get_army().get_ai_province() == ar.get_ai_province()) {
						if(ar.get_location_from_army_location() != o.get_army().get_location_from_army_location()) {
							// an army with the same target on a different location
							if(o.get_army().get_path().size() > 0 && o.get_army().get_path()[0] == ar.get_location_from_army_location()) {
								all_gathered = false;
								break;
							}
						} else {
							// on same location
							if(o.get_army().get_battle_from_army_battle_participation()) { // is in a battle
								all_gathered = false;
								break;
							}
						}
					}
				}
				if(all_gathered) {
					if(province::has_access_to_province(state, ar.get_controller_from_army_control(), ar.get_ai_province())) {
						if(ar.get_ai_province() == ar.get_location_from_army_location()) {
							for(auto o : ar.get_location_from_army_location().get_army_location()) {
								if(o.get_army().get_ai_province() == ar.get_ai_province()
								&& o.get_army().get_path().size() == 0) {
									o.get_army().set_ai_activity(uint8_t(army_activity::attack_gathered));
								}
							}
						} else if(auto path = province::make_land_path(state, ar.get_location_from_army_location(), ar.get_ai_province(), ar.get_controller_from_army_control(), ar); path.size() > 0) {
							for(auto o : ar.get_location_from_army_location().get_army_location()) {
								if(o.get_army().get_ai_province() == ar.get_ai_province()
								&& o.get_army().get_path().size() == 0) {

									auto existing_path = o.get_army().get_path();
									auto new_size = uint32_t(path.size());
									existing_path.resize(new_size);

									for(uint32_t i = 0; i < new_size; ++i) {
										assert(path[i]);
										existing_path[i] = path[i];
									}
									o.get_army().set_arrival_time(military::arrival_time_to(state, o.get_army(), path.back()));
									o.get_army().set_dig_in(0);
									o.get_army().set_ai_activity(uint8_t(army_activity::attack_gathered));
								}
							}
						} else {
							for(auto o : ar.get_location_from_army_location().get_army_location()) {
								if(o.get_army().get_ai_province() == ar.get_ai_province()
								&& o.get_army().get_path().size() == 0) {
									require_transport.push_back(o.get_army().id);
									ar.set_ai_activity(uint8_t(army_activity::attack_transport));
								}
							}
						}
					} else {
						ar.set_ai_activity(uint8_t(army_activity::on_guard));
					ar.set_ai_province(dcon::province_id{});
					}
				}
			}
		}

		for(uint32_t i = 0; i < require_transport.size(); ++i) {
			auto coastal_target_prov = state.world.army_get_location_from_army_location(require_transport[i]);
			auto controller = state.world.army_get_controller_from_army_control(require_transport[i]);

			dcon::navy_id transport_fleet = find_transport_fleet(state, controller);

			auto regs = state.world.army_get_army_membership(require_transport[i]);

			auto tcap = military::transport_capacity(state, transport_fleet);
			tcap -= int32_t(regs.end() - regs.begin());

			if(tcap < 0 || (state.world.nation_get_is_at_war(controller) && !naval_advantage(state, controller))) {
				for(uint32_t j = uint32_t(require_transport.size()); j-- > i + 1;) {
					if(state.world.army_get_controller_from_army_control(require_transport[j]) == controller) {
						state.world.army_set_ai_activity(require_transport[j], uint8_t(army_activity::on_guard));
						state.world.army_set_ai_province(require_transport[j], dcon::province_id{}); // stop rechecking these units
						require_transport[j] = require_transport.back();
						require_transport.pop_back();
					}
				}
				state.world.army_set_ai_activity(require_transport[i], uint8_t(army_activity::on_guard));
				state.world.army_set_ai_province(require_transport[i], dcon::province_id{}); // stop rechecking these units
				continue;
			}

			if(!state.world.province_get_is_coast(coastal_target_prov)) {
				auto path = province::make_path_to_nearest_coast(state, controller, coastal_target_prov);
				if(path.empty()) {
					state.world.army_set_ai_activity(require_transport[i], uint8_t(army_activity::on_guard));
				state.world.army_set_ai_province(require_transport[i], dcon::province_id{});
					continue; // army could not reach coast
				} else {
					coastal_target_prov = path.front();

					auto existing_path = state.world.army_get_path(require_transport[i]);
					auto new_size = uint32_t(path.size());
					existing_path.resize(new_size);

					for(uint32_t k = 0; k < new_size; ++k) {
						assert(path[k]);
						existing_path[k] = path[k];
					}
					state.world.army_set_arrival_time(require_transport[i], military::arrival_time_to(state, require_transport[i], path.back()));
					state.world.army_set_dig_in(require_transport[i], 0);
				}
			}

			{
				auto fleet_destination = province::has_naval_access_to_province(state, controller, coastal_target_prov) ? coastal_target_prov : state.world.province_get_port_to(coastal_target_prov);
				if(fleet_destination == state.world.navy_get_location_from_navy_location(transport_fleet)) {
					state.world.navy_get_path(transport_fleet).clear();
					state.world.navy_set_arrival_time(transport_fleet, sys::date{});
					state.world.navy_set_ai_activity(transport_fleet, uint8_t(fleet_activity::boarding));
				} else if(auto fleet_path = province::make_naval_path(state, state.world.navy_get_location_from_navy_location(transport_fleet), fleet_destination); fleet_path.empty()) {
					continue;
				} else {
					auto existing_path = state.world.navy_get_path(transport_fleet);
					auto new_size = uint32_t(fleet_path.size());
					existing_path.resize(new_size);

					for(uint32_t k = 0; k < new_size; ++k) {
						assert(fleet_path[k]);
						existing_path[k] = fleet_path[k];
					}
					state.world.navy_set_arrival_time(transport_fleet, military::arrival_time_to(state, transport_fleet, fleet_path.back()));
					state.world.navy_set_ai_activity(transport_fleet, uint8_t(fleet_activity::boarding));
				}
			}

			state.world.army_set_ai_activity(require_transport[i], uint8_t(army_activity::transport_attack));

			auto destination_region = state.world.province_get_connected_region_id(state.world.army_get_ai_province(require_transport[i]));

			// scoop up other armies to transport
			for(uint32_t j = uint32_t(require_transport.size()); j-- > i + 1;) {
				if(state.world.army_get_controller_from_army_control(require_transport[j]) == controller) {
					auto jregs = state.world.army_get_army_membership(require_transport[j]);
					if(tcap >= (jregs.end() - jregs.begin())) { // check if it will fit
						if(state.world.province_get_connected_region_id(state.world.army_get_ai_province(require_transport[j])) != destination_region)
						continue;

						if(state.world.army_get_location_from_army_location(require_transport[j]) == coastal_target_prov) {
							state.world.army_set_ai_activity(require_transport[i], uint8_t(army_activity::transport_attack));
							tcap -= int32_t(jregs.end() - jregs.begin());
						} else {
							auto jpath = state.world.army_get_black_flag(require_transport[j])
							? province::make_land_path(state, state.world.army_get_location_from_army_location(require_transport[j]), coastal_target_prov, controller, require_transport[j])
							: province::make_unowned_land_path(state, state.world.army_get_location_from_army_location(require_transport[j]), coastal_target_prov);
							if(!jpath.empty()) {
								auto existing_path = state.world.army_get_path(require_transport[j]);
								auto new_size = uint32_t(jpath.size());
								existing_path.resize(new_size);

								for(uint32_t k = 0; k < new_size; ++k) {
									assert(jpath[k]);
									existing_path[k] = jpath[k];
								}
								state.world.army_set_arrival_time(require_transport[j], military::arrival_time_to(state, require_transport[j], jpath.back()));
								state.world.army_set_dig_in(require_transport[j], 0);
								state.world.army_set_ai_activity(require_transport[i], uint8_t(army_activity::transport_attack));
								tcap -= int32_t(jregs.end() - jregs.begin());
							}
						}
					}

					require_transport[j] = require_transport.back();
					require_transport.pop_back();
				}
			}
		}
	}

	void update_land_constructions(sys::state& state) {
		for(auto n : state.world.in_nation) {
			if(n.get_is_player_controlled() || n.get_owned_province_count() == 0)
				continue;
			auto disarm = n.get_disarmed_until();
			if(disarm && state.current_date < disarm)
				continue;

			// TODO: Maybe make this a game rule?
			// Causes issues with compactable, so we just change to erasable!
			/*
			static std::vector<dcon::province_land_construction_id> hopeless_construction;
			hopeless_construction.clear();
			state.world.nation_for_each_province_land_construction(n, [&](dcon::province_land_construction_id plcid) {
				auto fat_plc = dcon::fatten(state.world, plcid);
				auto prov = fat_plc.get_pop().get_province_from_pop_location();
				if(prov.get_nation_from_province_control() != n) {
					hopeless_construction.push_back(plcid);
				}
			});
			for(auto item : hopeless_construction) {
				state.world.delete_province_land_construction(item);
			}
			*/

			auto constructions = state.world.nation_get_province_land_construction(n);
			if(constructions.begin() != constructions.end())
			continue;

			int32_t num_frontline = 0;
			int32_t num_support = 0;
			int32_t num_cavalry = 0;
			std::array<dcon::unit_type_id, 4> best_inf{
				state.military_definitions.irregular, //normal
				state.military_definitions.irregular, //build overseas
				state.military_definitions.irregular, //non-accepted
				state.military_definitions.irregular, //non-accepted + build overseas
			};
			std::array<dcon::unit_type_id, 4> best_art{
			dcon::unit_type_id{}, //normal
			dcon::unit_type_id{}, //build overseas
			dcon::unit_type_id{}, //non-accepted
			dcon::unit_type_id{}, //non-accepted + build overseas
			};
			std::array<dcon::unit_type_id, 4> best_cav{
			dcon::unit_type_id{}, //normal
			dcon::unit_type_id{}, //build overseas
			dcon::unit_type_id{}, //non-accepted
			dcon::unit_type_id{}, //non-accepted + build overseas
			};
			for(uint32_t i = 0; i < state.military_definitions.unit_base_definitions.size(); ++i) {
			dcon::unit_type_id utid{ dcon::unit_type_id::value_base_t(i) };
				if(!state.military_definitions.unit_base_definitions[utid].is_land
				|| (!n.get_active_unit(utid) && !state.military_definitions.unit_base_definitions[utid].active))
					continue;
				float s2 = estimate_unit_type_value(state, n, utid);
				if(state.military_definitions.unit_base_definitions[utid].type == military::unit_type::infantry) {
					for(uint32_t j = 0; j < 4; j++) {
						float s1 = best_inf[j] ? estimate_unit_type_value(state, n, best_inf[j]) : -1.f;
						if(s1 < s2) {
							bool b_ov = (j & 1) == 0 || state.military_definitions.unit_base_definitions[utid].can_build_overseas;
							bool b_pc = (j & 2) == 0 || !state.military_definitions.unit_base_definitions[utid].primary_culture;
							if(b_ov && b_pc)
								best_inf[j] = utid;
						}
					}
				} else if(state.military_definitions.unit_base_definitions[utid].type == military::unit_type::cavalry) {
					for(uint32_t j = 0; j < 4; j++) {
						float s1 = best_cav[j] ? estimate_unit_type_value(state, n, best_cav[j]) : -1.f;
						if(s1 < s2) {
							bool b_ov = (j & 1) == 0 || state.military_definitions.unit_base_definitions[utid].can_build_overseas;
							bool b_pc = (j & 2) == 0 || !state.military_definitions.unit_base_definitions[utid].primary_culture;
							if(b_ov && b_pc)
								best_cav[j] = utid;
						}
					}
				} else if(state.military_definitions.unit_base_definitions[utid].type == military::unit_type::support
				|| state.military_definitions.unit_base_definitions[utid].type == military::unit_type::special) {
					for(uint32_t j = 0; j < 4; j++) {
						float s1 = best_art[j] ? estimate_unit_type_value(state, n, best_art[j]) : -1.f;
						if(s1 < s2) {
							bool b_ov = (j & 1) == 0 || state.military_definitions.unit_base_definitions[utid].can_build_overseas;
							bool b_pc = (j & 2) == 0 || !state.military_definitions.unit_base_definitions[utid].primary_culture;
							if(b_ov && b_pc)
								best_art[j] = utid;
						}
					}
				}
			}

			for(auto ar : state.world.nation_get_army_control(n)) {
				for(auto r : ar.get_army().get_army_membership()) {
					auto type = r.get_regiment().get_type();
					assert(type);
					if(!type)
						continue;
					auto etype = state.military_definitions.unit_base_definitions[type].type;
					bool overseas = r.get_regiment().get_pop_from_regiment_source().get_province_from_pop_location().get_is_colonial();
					bool is_pc = nations::nation_accepts_culture(state, n, r.get_regiment().get_pop_from_regiment_source().get_culture());
					if(etype == military::unit_type::support
					|| etype == military::unit_type::special) {
						++num_support;
					} else if(etype == military::unit_type::cavalry) {
						++num_cavalry;
					} else {
						++num_frontline;
					}
				}
			}

			auto const decide_type = [&](dcon::pop_id pop, bool overseas) {
				bool is_pc = nations::nation_accepts_culture(state, n, state.world.pop_get_culture(pop));
				uint32_t index = (overseas ? 1 : 0) + (!is_pc ? 2 : 0);
				if(num_frontline > num_support && best_art[index])
					return best_art[index];
				if(num_frontline > (num_cavalry / 5) && best_cav[index])
					return best_cav[index];
				return best_inf[index];
			};

			for(auto p : state.world.nation_get_province_ownership(n)) {
				if(p.get_province().get_nation_from_province_control() != n)
					continue;

				bool overseas = province::is_overseas(state, p.get_province());
				if(p.get_province().get_is_colonial()) {
					float divisor = state.defines.pop_size_per_regiment * state.defines.pop_min_size_for_regiment_colony_multiplier;
					float minimum = state.defines.pop_min_size_for_regiment;
					for(auto pop : p.get_province().get_pop_location()) {
						if(pop.get_pop().get_poptype() == state.culture_definitions.soldiers) {
							if(pop.get_pop().get_size() >= minimum) {
								auto t = decide_type(pop.get_pop(), overseas);
								assert(t && state.military_definitions.unit_base_definitions[t].is_land);
								auto amount = int32_t((pop.get_pop().get_size() / divisor) + 1);
								auto regs = pop.get_pop().get_regiment_source();
								auto building = pop.get_pop().get_province_land_construction();
								auto num_to_make = amount - ((regs.end() - regs.begin()) + (building.end() - building.begin()));
								while(num_to_make > 0) {
									assert(command::can_start_land_unit_construction(state, n, pop.get_province(), pop.get_pop().get_culture(), t));
									auto c = fatten(state.world, state.world.try_create_province_land_construction(pop.get_pop().id, n));
									c.set_remaining_construction_time(state.military_definitions.unit_base_definitions[t].build_time);
									c.set_type(t);
									update_frontline_counters(state, t, num_frontline, num_support, num_cavalry);
									--num_to_make;
								}
							}
						}
					}
				} else if(!p.get_province().get_is_owner_core()) {
					float divisor = state.defines.pop_size_per_regiment * state.defines.pop_min_size_for_regiment_noncore_multiplier;
					float minimum = state.defines.pop_min_size_for_regiment;
					for(auto pop : p.get_province().get_pop_location()) {
						if(pop.get_pop().get_poptype() == state.culture_definitions.soldiers) {
							if(pop.get_pop().get_size() >= minimum) {
								auto t = decide_type(pop.get_pop(), overseas);
								assert(t && state.military_definitions.unit_base_definitions[t].is_land);
								auto amount = int32_t((pop.get_pop().get_size() / divisor) + 1);
								auto regs = pop.get_pop().get_regiment_source();
								auto building = pop.get_pop().get_province_land_construction();
								auto num_to_make = amount - ((regs.end() - regs.begin()) + (building.end() - building.begin()));
								while(num_to_make > 0) {
									assert(command::can_start_land_unit_construction(state, n, pop.get_province(), pop.get_pop().get_culture(), t));
									auto c = fatten(state.world, state.world.try_create_province_land_construction(pop.get_pop().id, n));
									c.set_remaining_construction_time(state.military_definitions.unit_base_definitions[t].build_time);
									c.set_type(t);
									update_frontline_counters(state, t, num_frontline, num_support, num_cavalry);
									--num_to_make;
								}
							}
						}
					}
				} else {
					float divisor = state.defines.pop_size_per_regiment;
					float minimum = state.defines.pop_min_size_for_regiment;
					for(auto pop : p.get_province().get_pop_location()) {
						if(pop.get_pop().get_poptype() == state.culture_definitions.soldiers) {
							if(pop.get_pop().get_size() >= minimum) {
								auto t = decide_type(pop.get_pop(), overseas);
								assert(t && state.military_definitions.unit_base_definitions[t].is_land);
								auto amount = int32_t((pop.get_pop().get_size() / divisor) + 1);
								auto regs = pop.get_pop().get_regiment_source();
								auto building = pop.get_pop().get_province_land_construction();
								auto num_to_make = amount - ((regs.end() - regs.begin()) + (building.end() - building.begin()));
								while(num_to_make > 0) {
									assert(command::can_start_land_unit_construction(state, n, pop.get_province(), pop.get_pop().get_culture(), t));
									auto c = fatten(state.world, state.world.try_create_province_land_construction(pop.get_pop().id, n));
									c.set_remaining_construction_time(state.military_definitions.unit_base_definitions[t].build_time);
									c.set_type(t);
									update_frontline_counters(state, t, num_frontline, num_support, num_cavalry);
									--num_to_make;
								}
							}
						}
					}
				}
			}
		}
	}

	void new_units_and_merging(sys::state& state) {
		for(auto ar : state.world.in_army) {
			auto controller = ar.get_controller_from_army_control();
			if(controller
			&& unit_on_ai_control(state, ar)
			&& !ar.get_battle_from_army_battle_participation()
			&& !ar.get_navy_from_army_transport()
			&& !ar.get_arrival_time()) {

				auto location = ar.get_location_from_army_location();

				if(ar.get_black_flag() || army_activity(ar.get_ai_activity()) == army_activity::unspecified) {
					auto regs = ar.get_army_membership();
					if(regs.begin() == regs.end()) {
						// empty army -- cleanup will get it
					} else if(regs.end() - regs.begin() > 1) {
						// existing multi-unit formation
						ar.set_ai_activity(uint8_t(army_activity::on_guard));
					} else {
						auto art_type = state.military_definitions.unit_base_definitions[(*regs.begin()).get_regiment().get_type()].type;
						bool is_art = art_type == military::unit_type::support || art_type == military::unit_type::special;
						bool is_cav = art_type == military::unit_type::cavalry;
						dcon::province_id target_location;
						float nearest_distance = 1.0f;

						// find army to merge with
						for(auto o : controller.get_army_control()) {
							auto other_location = o.get_army().get_location_from_army_location();
							auto sdist = province::sorting_distance(state, other_location, location);
							if(army_activity(o.get_army().get_ai_activity()) == army_activity::on_guard
							&& other_location.get_connected_region_id() == location.get_connected_region_id()
							&& (!target_location || sdist < nearest_distance)) {

								int32_t num_support = 0;
								int32_t num_frontline = 0;
								int32_t num_cavalry = 0;
								for(auto r : o.get_army().get_army_membership()) {
									auto type = r.get_regiment().get_type();
									auto etype = state.military_definitions.unit_base_definitions[type].type;
									if(etype == military::unit_type::support || etype == military::unit_type::special) {
										++num_support;
									} else if(etype == military::unit_type::cavalry) {
										++num_cavalry;
									} else {
										++num_frontline;
									}
								}
								// 4/1/5 metastack
								if((is_art && num_support < 5)
								|| (!is_art && num_frontline < 4)
								|| (is_cav && num_cavalry < 1)) {
									target_location = other_location;
									nearest_distance = sdist;
								}
							}
						}

						if(target_location) {
							if(target_location == location) {
								ar.set_ai_province(target_location);
								ar.set_ai_activity(uint8_t(army_activity::merging));
							} else if(auto path = province::make_land_path(state, location, target_location, controller, ar); path.size() > 0) {
								auto existing_path = ar.get_path();
								auto new_size = uint32_t(path.size());
								existing_path.resize(new_size);

								for(uint32_t i = 0; i < new_size; ++i) {
									assert(path[i]);
									existing_path[i] = path[i];
								}
								ar.set_arrival_time(military::arrival_time_to(state, ar, path.back()));
								ar.set_dig_in(0);
								ar.set_ai_province(target_location);
								ar.set_ai_activity(uint8_t(army_activity::merging));
							} else {
								ar.set_ai_activity(uint8_t(army_activity::on_guard));
							}
						} else {
							ar.set_ai_activity(uint8_t(army_activity::on_guard));
						}
					}
				} else if(army_activity(ar.get_ai_activity()) == army_activity::merging) {
					auto regs = ar.get_army_membership();
					if(regs.begin() == regs.end()) {
						// empty army -- cleanup will get it
						continue;
					}
					auto art_type = state.military_definitions.unit_base_definitions[(*regs.begin()).get_regiment().get_type()].type;
					bool is_art = art_type == military::unit_type::support || art_type == military::unit_type::special;
					bool is_cav = art_type == military::unit_type::cavalry;
					for(auto o : location.get_army_location()) {
						if(o.get_army().get_ai_activity() == uint8_t(army_activity::on_guard)
						&& o.get_army().get_controller_from_army_control() == controller) {

							int32_t num_support = 0;
							int32_t num_frontline = 0;
							int32_t num_cavalry = 0;
							for(auto r : o.get_army().get_army_membership()) {
								auto type = r.get_regiment().get_type();
								auto etype = state.military_definitions.unit_base_definitions[type].type;
								if(etype == military::unit_type::support || etype == military::unit_type::special) {
									++num_support;
								} else if(etype == military::unit_type::cavalry) {
									++num_cavalry;
								} else {
									++num_frontline;
								}
							}
							// 4/1/5 metastack
							if((is_art && num_support < 5)
							|| (!is_art && num_frontline < 4)
							|| (is_cav && num_cavalry < 1)) {
								(*regs.begin()).get_regiment().set_army_from_army_membership(o.get_army());
								break;
							}
						}
					}
					ar.set_ai_activity(uint8_t(army_activity::unspecified)); // if merging fails, this will try to find a merge target again
				}
			}
		}

	}

	void general_ai_unit_tick(sys::state& state) {
		auto v = state.current_date.value;
		auto r = v % 8;

		switch(r) {
		case 0:
			pickup_idle_ships(state);
			break;
		case 1:
			move_idle_guards(state);
			break;
		case 2:
			new_units_and_merging(state);
			break;
		case 3:
			move_gathered_attackers(state);
			break;
		case 4:
			update_naval_transport(state);
			break;
		case 5:
			move_idle_guards(state);
			break;
		case 6:
			break;
		case 7:
			move_gathered_attackers(state);
			break;
		}
	}

	float estimate_rebel_strength(sys::state& state, dcon::province_id p) {
		float v = 0.f;
		for(auto ar : state.world.province_get_army_location(p)) {
			if(ar.get_army().get_controller_from_army_rebel_control()) {
				v += estimate_army_defensive_strength(state, ar.get_army());
			}
		}
		return v;
	}

	// Easy way to inflate our industrial score
	void perform_foreign_investments(sys::state& state) {
		//Ensure we're atleast x3 of the total cost, aka. we have A LOT OF MONEY lying around for us to spend
		constexpr float invest_safety_factor = 5.f;
		// Spam railroads whenever possible
		for(auto gprl : state.world.in_gp_relationship) {
			if(gprl.get_great_power().get_is_player_controlled()) {
				// nothing -- player GP
			} else {
				auto gp = gprl.get_great_power();
				if((gp.get_combined_issue_rules() & issue_rule::allow_foreign_investment) == 0) {
					continue;
				}
				if(!gprl.get_influence_target().get_is_civilized()) {
					continue;
				}
				if((gprl.get_status() & nations::influence::level_mask) == nations::influence::level_friendly
				|| (gprl.get_status() & nations::influence::level_mask) == nations::influence::level_in_sphere) {
					continue;
				}
				{ //spam railroads!
					auto amount = 0.0f;
					auto const& base_cost = state.world.province_building_type_get_cost(state.economy_definitions.railroad_building);
					for(uint32_t j = 0; j < economy::commodity_set::set_size; ++j) {
						if(base_cost.commodity_type[j]) {
							amount += base_cost.commodity_amounts[j] * state.world.commodity_get_current_price(base_cost.commodity_type[j]);
						} else {
							break;
						}
					}
					int32_t max_rails_lvl = gprl.get_great_power().get_max_building_level(state.economy_definitions.railroad_building);
					for(const auto p : gprl.get_influence_target().get_province_ownership()) {
						int32_t current_rails_lvl = p.get_province().get_building_level(state.economy_definitions.railroad_building);
						int32_t min_build_railroad = int32_t(p.get_province().get_modifier_values(sys::provincial_mod_offsets::min_build_railroad));
						if((max_rails_lvl - current_rails_lvl - min_build_railroad > 0) && !province::has_railroads_being_built(state, p.get_province())) {
							auto money = nations::get_treasury(state, gprl.get_great_power());
							if(money >= amount * invest_safety_factor) {
								if(command::can_begin_province_building_construction(state, gprl.get_great_power(), p.get_province(), state.economy_definitions.railroad_building)) {
									command::execute_begin_province_building_construction(state, gprl.get_great_power(), p.get_province(), state.economy_definitions.railroad_building);
								}
							}
						}
					}
				}
				// get factory types our target may want
				std::vector<dcon::factory_type_id> desired_types;
				get_desired_factory_types(state, gprl.get_influence_target(), desired_types);
				for(const auto ft : desired_types) {
					if(!state.world.factory_type_get_is_available_from_start(ft)
					&& !state.world.nation_get_active_building(gprl.get_great_power(), ft))
						continue;
					auto amount = 0.0f;
					auto const& base_cost = state.world.factory_type_get_construction_costs(ft);
					for(uint32_t j = 0; j < economy::commodity_set::set_size; ++j) {
						if(base_cost.commodity_type[j]) {
							amount += base_cost.commodity_amounts[j] * state.world.commodity_get_current_price(base_cost.commodity_type[j]);
						} else {
							break;
						}
					}
					for(const auto s : gprl.get_influence_target().get_state_ownership()) {
						auto money = nations::get_treasury(state, gprl.get_great_power());
						if(money >= amount * invest_safety_factor) {
							if(command::can_begin_factory_building_construction(state, gprl.get_great_power(), s.get_state(), ft, true)) {
								command::execute_begin_factory_building_construction(state, gprl.get_great_power(), s.get_state(), ft, true);
							} else if(command::can_begin_factory_building_construction(state, gprl.get_great_power(), s.get_state(), ft, false)) {
								command::execute_begin_factory_building_construction(state, gprl.get_great_power(), s.get_state(), ft, false);
							}
						}
					}
				}
			}
		}
	}
}
