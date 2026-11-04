/*
 * town_event.h - random hamlet events (a plague, a festival, a bargain)
 * that change prices, recruits, stress relief and so on for the week.
 * Town events (campaign/town_events): one hamlet-wide modifier rolled each town visit. */
#ifndef DD_TOWN_EVENT_H
#define DD_TOWN_EVENT_H
#include "campaign.h"
#include "rng.h"

/* Active event this week, or NULL. */
const DDTownEvent *town_event(void);
/* Rolls the event for the new week and applies its recruit effects (call after recruits refresh). */
void town_event_roll(Rng *r);
/* Puts plot quest dd_plot_quests[pq_index] (PQ_INVASION, PQ_CROW) on the quest board. */
void town_event_post_quest(int pq_index);
/* Queues the event a finished quest guarantees (quest_type_event_guarantees.json). */
void town_event_quest_done(int dungeon, int quest_type);
/* First matching op of the active event (arg compared in full), or NULL. */
const DDTownEventOp *town_event_op(int type, int arg);
/* Town activity (TA_*): 1 if closed by the event. */
int town_event_locked(int activity);
/* Activity/treatment cost after the event (free, discount). */
int town_event_cost(int activity, int cost);
/* Stress relief of an activity for hero h after the event's buffs. */
int town_event_relief(int activity, int roster_index, int relief);
/* Provision price after the event (supply = every item but food). */
int town_event_item_price(int item, int price);
/* Upgrade cost: tag 0 armour, 1 weapon, 2 building, 3 trinket. Free upgrades are used by town_event_paid. */
int town_event_upgrade_cost(int tag, int cost);
void town_event_paid(int tag);
/* Embark: buffs on the party, resolve levels for idle heroes, remembers the event on the dungeon. */
void town_event_embark(Dungeon *d);
#endif
