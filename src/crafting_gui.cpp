#include "crafting_gui.h"

#include "crafting.h"
#include "recipe_dictionary.h"
#include "requirements.h"
#include "player.h"
#include "itype.h"
#include "input.h"
#include "game.h"
#include "translations.h"
#include "catacharset.h"
#include "output.h"
#include "cata_utility.h"

#include "debug.h"

#include <algorithm>
#include <string>
#include <vector>
#include <map>

enum TAB_MODE {
    NORMAL,
    FILTERED,
    BATCH
};

std::vector<std::string> craft_cat_list;
std::map<std::string, std::vector<std::string> > craft_subcat_list;
std::map<std::string, std::string> normalized_names;

static void draw_can_craft_indicator( WINDOW *w, const int margin_y, const recipe &rec );
static void draw_recipe_tabs( WINDOW *w, std::string tab, TAB_MODE mode = NORMAL );
static void draw_recipe_subtabs( WINDOW *w, std::string tab, std::string subtab,
                                 TAB_MODE mode = NORMAL );

std::string get_cat_name( std::string prefixed_name )
{
    return prefixed_name.substr( 3, prefixed_name.size() - 3 );
}

void load_recipe_category( JsonObject &jsobj )
{
    JsonArray subcats;
    std::string category = jsobj.get_string( "id" );

    if( category.find( "CC_" ) != 0 ) {
        jsobj.throw_error( "Crafting category id has to be prefixed with 'CC_'" );
    }

    craft_cat_list.push_back( category );

    std::string cat_name = get_cat_name( category );

    craft_subcat_list[category] = std::vector<std::string>();
    subcats = jsobj.get_array( "recipe_subcategories" );
    while( subcats.has_more() ) {
        std::string subcat_id = subcats.next_string();
        if( subcat_id.find( "CSC_" + cat_name + "_" ) != 0 && subcat_id != "CSC_ALL" ) {
            jsobj.throw_error( "Crafting sub-category id has to be prefixed with CSC_<category_name>_" );
        }
        craft_subcat_list[category].push_back( subcat_id );
    }
}

std::string get_subcat_name( const std::string &cat, std::string prefixed_name )
{
    std::string prefix = "CSC_" + get_cat_name( cat ) + "_";

    if( prefixed_name.find( prefix ) == 0 ) {
        return prefixed_name.substr( prefix.size(), prefixed_name.size() - prefix.size() );
    }

    return ( prefixed_name == "CSC_ALL" ? _( "ALL" ) : _( "NONCRAFT" ) );
}

void translate_all()
{
    for( const auto &cat : craft_cat_list ) {
        normalized_names[cat] = _( get_cat_name( cat ).c_str() );

        for( const auto &subcat : craft_subcat_list[cat] ) {
            normalized_names[subcat] =  _( get_subcat_name( cat, subcat ).c_str() ) ;
        }
    }
}

void reset_recipe_categories()
{
    craft_cat_list.clear();
    craft_subcat_list.clear();
}

const recipe *select_crafting_recipe( int &batch_size )
{
    if( normalized_names.empty() ) {
        translate_all();
    }

    const int headHeight = 3;
    const int subHeadHeight = 2;
    const int freeWidth = TERMX - FULL_SCREEN_WIDTH;
    bool isWide = ( TERMX > FULL_SCREEN_WIDTH && freeWidth > 15 );

    const int width = isWide ? ( freeWidth > FULL_SCREEN_WIDTH ? FULL_SCREEN_WIDTH * 2 : TERMX ) :
                      FULL_SCREEN_WIDTH;
    const int wStart = ( TERMX - width ) / 2;
    const int tailHeight = isWide ? 3 : 4;
    const int dataLines = TERMY - ( headHeight + subHeadHeight ) - tailHeight;
    const int dataHalfLines = dataLines / 2;
    const int dataHeight = TERMY - ( headHeight + subHeadHeight );
    const int infoWidth = width - FULL_SCREEN_WIDTH - 1;

    const recipe *last_recipe = nullptr;

    WINDOW *w_head = newwin( headHeight, width, 0, wStart );
    WINDOW_PTR w_head_ptr( w_head );
    WINDOW *w_subhead = newwin( subHeadHeight, width, 3, wStart );
    WINDOW_PTR w_subhead_ptr( w_subhead );
    WINDOW *w_data = newwin( dataHeight, width, headHeight + subHeadHeight, wStart );
    WINDOW_PTR w_data_ptr( w_data );

    int item_info_x = infoWidth;
    int item_info_y = dataHeight - 3;
    int item_info_width = wStart + width - infoWidth;
    int item_info_height = headHeight + subHeadHeight;

    if( !isWide ) {
        item_info_x = 1;
        item_info_y = 1;
        item_info_width = 1;
        item_info_height = 1;
    }

    WINDOW *w_iteminfo = newwin( item_info_y, item_info_x, item_info_height, item_info_width );
    WINDOW_PTR w_iteminfo_ptr( w_iteminfo );

    list_circularizer<std::string> tab( craft_cat_list );
    list_circularizer<std::string> subtab( craft_subcat_list[tab.cur()] );
    std::vector<const recipe *> current;
    std::vector<bool> available;
    const int componentPrintHeight = dataHeight - tailHeight - 1;
    //preserves component color printout between mode rotations
    nc_color rotated_color = c_white;
    int previous_item_line = -1;
    std::string previous_tab = "";
    std::string previous_subtab = "";
    item tmp;
    int line = 0, ypos, scroll_pos = 0;
    bool redraw = true;
    bool keepline = false;
    bool done = false;
    bool batch = false;
    int batch_line = 0;
    int display_mode = 0;
    const recipe *chosen = NULL;
    std::vector<iteminfo> thisItem, dummy;

    input_context ctxt( "CRAFTING" );
    ctxt.register_cardinal();
    ctxt.register_action( "QUIT" );
    ctxt.register_action( "CONFIRM" );
    ctxt.register_action( "CYCLE_MODE" );
    ctxt.register_action( "SCROLL_UP" );
    ctxt.register_action( "SCROLL_DOWN" );
    ctxt.register_action( "PREV_TAB" );
    ctxt.register_action( "NEXT_TAB" );
    ctxt.register_action( "FILTER" );
    ctxt.register_action( "RESET_FILTER" );
    ctxt.register_action( "HELP_RECIPE" );
    ctxt.register_action( "HELP_KEYBINDINGS" );
    ctxt.register_action( "CYCLE_BATCH" );

    const inventory &crafting_inv = g->u.crafting_inventory();
    const std::vector<npc *> helpers = g->u.get_crafting_helpers();
    std::string filterstring = "";

    std::map<const recipe *, bool> availability_cache;
    do {
        if( redraw ) {
            // When we switch tabs, redraw the header
            redraw = false;
            if( ! keepline ) {
                line = 0;
            } else {
                keepline = false;
            }

            if( display_mode > 2 ) {
                display_mode = 2;
            }

            TAB_MODE m = ( batch ) ? BATCH : ( filterstring == "" ) ? NORMAL : FILTERED;
            draw_recipe_tabs( w_head, tab.cur(), m );
            draw_recipe_subtabs( w_subhead, tab.cur(), subtab.cur(), m );
            current.clear();
            available.clear();
            if( batch ) {
                batch_recipes( crafting_inv, helpers, current, available, chosen );
            } else {

                if( filterstring.empty() ) {
                    current = recipe_dict.in_category( tab.cur(), subtab.cur() != "CSC_ALL" ? subtab.cur() : "" );
                } else {
                    current = recipe_dict.search( filterstring );
                }

                // cache recipe availability on first display
                for( const auto e : current ) {
                    if( !availability_cache.count( e ) ) {
                        availability_cache.emplace( e, e->can_make_with_inventory( crafting_inv, helpers ) );
                    }
                }

                std::stable_sort( current.begin(), current.end(), []( const recipe * a, const recipe * b ) {
                    return b->difficulty < a->difficulty;
                } );

                std::stable_sort( current.begin(), current.end(), [&]( const recipe * a, const recipe * b ) {
                    return availability_cache[a] && !availability_cache[b];
                } );

                std::transform( current.begin(), current.end(),
                std::back_inserter( available ), [&]( const recipe * e ) {
                    return availability_cache[e];
                } );
            }
        }

        // Clear the screen of recipe data, and draw it anew
        werase( w_data );

        if( isWide ) {
            werase( w_iteminfo );
        }

        if( isWide ) {
            mvwprintz( w_data, dataLines + 1, 5, c_white,
                       _( "Press <ENTER> to attempt to craft object." ) );
            wprintz( w_data, c_white, "  " );
            if( filterstring != "" ) {
                wprintz( w_data, c_white, _( "[E]: Describe, [F]ind, [R]eset, [m]ode, %s [?] keybindings" ),
                         ( batch ) ? _( "cancel [b]atch" ) : _( "[b]atch" ) );
            } else {
                wprintz( w_data, c_white, _( "[E]: Describe, [F]ind, [m]ode, %s [?] keybindings" ),
                         ( batch ) ? _( "cancel [b]atch" ) : _( "[b]atch" ) );
            }
        } else {
            if( filterstring != "" ) {
                mvwprintz( w_data, dataLines + 1, 5, c_white,
                           _( "[E]: Describe, [F]ind, [R]eset, [m]ode, [b]atch [?] keybindings" ) );
            } else {
                mvwprintz( w_data, dataLines + 1, 5, c_white,
                           _( "[E]: Describe, [F]ind, [m]ode, [b]atch [?] keybindings" ) );
            }
            mvwprintz( w_data, dataLines + 2, 5, c_white,
                       _( "Press <ENTER> to attempt to craft object." ) );
        }
        // Draw borders
        for( int i = 1; i < width - 1; ++i ) { // _
            mvwputch( w_data, dataHeight - 1, i, BORDER_COLOR, LINE_OXOX );
        }
        for( int i = 0; i < dataHeight - 1; ++i ) { // |
            mvwputch( w_data, i, 0, BORDER_COLOR, LINE_XOXO );
            mvwputch( w_data, i, width - 1, BORDER_COLOR, LINE_XOXO );
        }
        mvwputch( w_data, dataHeight - 1,  0, BORDER_COLOR, LINE_XXOO ); // _|
        mvwputch( w_data, dataHeight - 1, width - 1, BORDER_COLOR, LINE_XOOX ); // |_

        int recmin = 0, recmax = current.size();
        if( recmax > dataLines ) {
            if( line <= recmin + dataHalfLines ) {
                for( int i = recmin; i < recmin + dataLines; ++i ) {
                    std::string tmp_name = item::nname( current[i]->result );
                    if( batch ) {
                        tmp_name = string_format( _( "%2dx %s" ), i + 1, tmp_name.c_str() );
                    }
                    mvwprintz( w_data, i - recmin, 2, c_dkgray, "" ); // Clear the line
                    if( i == line ) {
                        mvwprintz( w_data, i - recmin, 2, ( available[i] ? h_white : h_dkgray ),
                                   utf8_truncate( tmp_name, 28 ).c_str() );
                    } else {
                        mvwprintz( w_data, i - recmin, 2, ( available[i] ? c_white : c_dkgray ),
                                   utf8_truncate( tmp_name, 28 ).c_str() );
                    }
                }
            } else if( line >= recmax - dataHalfLines ) {
                for( int i = recmax - dataLines; i < recmax; ++i ) {
                    std::string tmp_name = item::nname( current[i]->result );
                    if( batch ) {
                        tmp_name = string_format( _( "%2dx %s" ), i + 1, tmp_name.c_str() );
                    }
                    mvwprintz( w_data, dataLines + i - recmax, 2, c_ltgray, "" ); // Clear the line
                    if( i == line ) {
                        mvwprintz( w_data, dataLines + i - recmax, 2,
                                   ( available[i] ? h_white : h_dkgray ),
                                   utf8_truncate( tmp_name, 28 ).c_str() );
                    } else {
                        mvwprintz( w_data, dataLines + i - recmax, 2,
                                   ( available[i] ? c_white : c_dkgray ),
                                   utf8_truncate( tmp_name, 28 ).c_str() );
                    }
                }
            } else {
                for( int i = line - dataHalfLines; i < line - dataHalfLines + dataLines; ++i ) {
                    std::string tmp_name = item::nname( current[i]->result );
                    if( batch ) {
                        tmp_name = string_format( _( "%2dx %s" ), i + 1, tmp_name.c_str() );
                    }
                    mvwprintz( w_data, dataHalfLines + i - line, 2, c_ltgray, "" ); // Clear the line
                    if( i == line ) {
                        mvwprintz( w_data, dataHalfLines + i - line, 2,
                                   ( available[i] ? h_white : h_dkgray ),
                                   utf8_truncate( tmp_name, 28 ).c_str() );
                    } else {
                        mvwprintz( w_data, dataHalfLines + i - line, 2,
                                   ( available[i] ? c_white : c_dkgray ),
                                   utf8_truncate( tmp_name, 28 ).c_str() );
                    }
                }
            }
        } else {
            for( size_t i = 0; i < current.size() && i < ( size_t )dataHeight + 1; ++i ) {
                std::string tmp_name = item::nname( current[i]->result );
                if( batch ) {
                    tmp_name = string_format( _( "%2dx %s" ), ( int )i + 1, tmp_name.c_str() );
                }
                if( ( int )i == line ) {
                    mvwprintz( w_data, i, 2, ( available[i] ? h_white : h_dkgray ),
                               utf8_truncate( tmp_name, 28 ).c_str() );
                } else {
                    mvwprintz( w_data, i, 2, ( available[i] ? c_white : c_dkgray ),
                               utf8_truncate( tmp_name, 28 ).c_str() );
                }
            }
        }

        if( !current.empty() ) {
            int pane = FULL_SCREEN_WIDTH - 30 - 1;
            int count = batch ? line + 1 : 1; // batch size
            nc_color col = available[ line ] ? c_white : c_ltgray;

            const auto &req = current[ line ]->requirements();

            draw_can_craft_indicator( w_head, 0, *current[line] );
            wrefresh( w_head );

            ypos = 0;

            std::vector<std::string> component_print_buffer;
            auto tools = req.get_folded_tools_list( pane, col, crafting_inv, count );
            auto comps = req.get_folded_components_list( pane, col, crafting_inv, count );
            component_print_buffer.insert( component_print_buffer.end(), tools.begin(), tools.end() );
            component_print_buffer.insert( component_print_buffer.end(), comps.begin(), comps.end() );

            if( !g->u.knows_recipe( current[line] ) ) {
                component_print_buffer.push_back( _( "Recipe not memorized yet" ) );
            }

            //handle positioning of component list if it needed to be scrolled
            int componentPrintOffset = 0;
            if( display_mode > 2 ) {
                componentPrintOffset = ( display_mode - 2 ) * componentPrintHeight;
            }
            if( component_print_buffer.size() < static_cast<size_t>( componentPrintOffset ) ) {
                componentPrintOffset = 0;
                if( previous_tab != tab.cur() || previous_subtab != subtab.cur() || previous_item_line != line ) {
                    display_mode = 2;
                } else {
                    display_mode = 0;
                }
            }

            //only used to preserve mode position on components when
            //moving to another item and the view is already scrolled
            previous_tab = tab.cur();
            previous_subtab = subtab.cur();
            previous_item_line = line;

            if( display_mode == 0 ) {
                mvwprintz( w_data, ypos++, 30, col, _( "Skills used: %s" ),
                           ( !current[line]->skill_used ? _( "N/A" ) :
                             current[line]->skill_used.obj().name().c_str() ) );

                mvwprintz( w_data, ypos++, 30, col, _( "Required skills: %s" ),
                           ( current[line]->required_skills_string().c_str() ) );
                mvwprintz( w_data, ypos++, 30, col, _( "Difficulty: %d" ), current[line]->difficulty );
                if( !current[line]->skill_used ) {
                    mvwprintz( w_data, ypos++, 30, col, _( "Your skill level: N/A" ) );
                } else {
                    mvwprintz( w_data, ypos++, 30, col, _( "Your skill level: %d" ),
                               // Macs don't seem to like passing this as a class, so force it to int
                               ( int )g->u.get_skill_level( current[line]->skill_used ) );
                }
                ypos += current[line]->print_time( w_data, ypos, 30, pane, col, count );
                mvwprintz( w_data, ypos++, 30, col, _( "Dark craftable? %s" ),
                           current[line]->has_flag( "BLIND_EASY" ) ? _( "Easy" ) :
                           current[line]->has_flag( "BLIND_HARD" ) ? _( "Hard" ) :
                           _( "Impossible" ) );
                ypos += current[line]->print_items( w_data, ypos, 30, col, ( batch ) ? line + 1 : 1 );
            }

            //color needs to be preserved in case part of the previous page was cut off
            nc_color stored_color = col;
            if( display_mode > 2 ) {
                stored_color = rotated_color;
            } else {
                rotated_color = col;
            }
            int components_printed = 0;
            for( size_t i = static_cast<size_t>( componentPrintOffset );
                 i < component_print_buffer.size(); i++ ) {
                if( ypos >= componentPrintHeight ) {
                    break;
                }

                components_printed++;
                print_colored_text( w_data, ypos++, 30, stored_color, col, component_print_buffer[i] );
            }

            if( ypos >= componentPrintHeight &&
                component_print_buffer.size() > static_cast<size_t>( components_printed ) ) {
                mvwprintz( w_data, ypos++, 30, col, _( "v (more)" ) );
                rotated_color = stored_color;
            }

            if( isWide ) {
                if( last_recipe != current[line] ) {
                    last_recipe = current[line];
                    tmp = current[line]->create_result();
                    tmp.info( true, thisItem );
                }
                draw_item_info( w_iteminfo, tmp.tname(), tmp.type_name(), thisItem, dummy,
                                scroll_pos, true, true, true, false, true );
            }
        }

        draw_scrollbar( w_data, line, dataLines, recmax, 0 );
        wrefresh( w_data );

        if( isWide ) {
            wrefresh( w_iteminfo );
        }

        const std::string action = ctxt.handle_input();
        if( action == "CYCLE_MODE" ) {
            display_mode = display_mode + 1;
            if( display_mode <= 0 ) {
                display_mode = 0;
            }
        } else if( action == "LEFT" ) {
            subtab.prev();
            redraw = true;
        } else if( action == "SCROLL_UP" ) {
            scroll_pos--;
        } else if( action == "SCROLL_DOWN" ) {
            scroll_pos++;
        } else if( action == "PREV_TAB" ) {
            tab.prev();
            subtab = list_circularizer<std::string>( craft_subcat_list[tab.cur()] );//default ALL
            redraw = true;
        } else if( action == "RIGHT" ) {
            subtab.next();
            redraw = true;
        } else if( action == "NEXT_TAB" ) {
            tab.next();
            subtab = list_circularizer<std::string>( craft_subcat_list[tab.cur()] );//default ALL
            redraw = true;
        } else if( action == "DOWN" ) {
            line++;
        } else if( action == "UP" ) {
            line--;
        } else if( action == "CONFIRM" ) {
            if( available.empty() || !available[line] ) {
                popup( _( "You can't do that!" ) );
            } else if( !current[line]->check_eligible_containers_for_crafting( ( batch ) ? line + 1 : 1 ) ) {
                ; // popup is already inside check
            } else {
                chosen = current[line];
                batch_size = ( batch ) ? line + 1 : 1;
                done = true;
            }
        } else if( action == "HELP_RECIPE" ) {
            if( current.empty() ) {
                popup( _( "Nothing selected!" ) );
                redraw = true;
                continue;
            }
            tmp = current[line]->create_result();

            full_screen_popup( "%s\n%s", tmp.type_name( 1 ).c_str(),  tmp.info( true ).c_str() );
            redraw = true;
            keepline = true;
        } else if( action == "FILTER" ) {
            filterstring = string_input_popup( _( "Search:" ), 85, filterstring,
                                               _( "Special prefixes for requirements:\n"
                                                  "  [t] search tools\n"
                                                  "  [c] search components\n"
                                                  "  [q] search qualities\n"
                                                  "  [s] search skills\n"
                                                  "  [S] search skill used only\n"
                                                  "Special prefixes for results:\n"
                                                  "  [Q] search qualities\n"
                                                  "Examples:\n"
                                                  "  t:soldering iron\n"
                                                  "  c:two by four\n"
                                                  "  q:metal sawing\n"
                                                  "  s:cooking\n"
                                                  "  Q:fine bolt turning"
                                                ) );
            redraw = true;
        } else if( action == "QUIT" ) {
            chosen = nullptr;
            done = true;
        } else if( action == "RESET_FILTER" ) {
            filterstring = "";
            redraw = true;
        } else if( action == "CYCLE_BATCH" ) {
            if( current.empty() ) {
                popup( _( "Nothing selected!" ) );
                redraw = true;
                continue;
            }
            batch = !batch;
            if( batch ) {
                batch_line = line;
                chosen = current[batch_line];
            } else {
                line = batch_line;
                keepline = true;
            }
            redraw = true;
        }
        if( line < 0 ) {
            line = current.size() - 1;
        } else if( line >= ( int )current.size() ) {
            line = 0;
        }
    } while( !done );

    return chosen;
}

// Anchors top-right
static void draw_can_craft_indicator( WINDOW *w, const int margin_y, const recipe &rec )
{
    // Erase previous text
    // @fixme replace this hack by proper solution (based on max width of possible content)
    right_print( w, margin_y + 1, 1, c_black, "        " );
    // Draw text
    right_print( w, margin_y, 1, c_ltgray, "%s", _( "can craft:" ) );
    if( g->u.lighting_craft_speed_multiplier( rec ) == 0.0f ) {
        right_print( w, margin_y + 1, 1, i_red, "%s", _( "too dark" ) );
    } else if( !g->u.has_morale_to_craft() ) {
        right_print( w, margin_y + 1, 1, i_red, "%s", _( "too sad" ) );
    } else if( g->u.lighting_craft_speed_multiplier( rec ) < 1.0f ) {
        right_print( w, margin_y + 1, 1, i_yellow, _( "slow %d%%" ),
                     int( g->u.lighting_craft_speed_multiplier( rec ) * 100 ) );
    } else {
        right_print( w, margin_y + 1, 1, i_green, "%s", _( "yes" ) );
    }
}

static void draw_recipe_tabs( WINDOW *w, std::string tab, TAB_MODE mode )
{
    werase( w );
    int width = getmaxx( w );
    for( int i = 0; i < width; i++ ) {
        mvwputch( w, 2, i, BORDER_COLOR, LINE_OXOX );
    }

    mvwputch( w, 2,  0, BORDER_COLOR, LINE_OXXO ); // |^
    mvwputch( w, 2, width - 1, BORDER_COLOR, LINE_OOXX ); // ^|

    switch( mode ) {
        case NORMAL: {
            int pos_x = 2;//draw the tabs on each other
            int tab_step = 3;//step between tabs, two for tabs border
            for( const auto &tt : craft_cat_list ) {
                draw_tab( w, pos_x, normalized_names[tt], tab == tt );
                pos_x += utf8_width( normalized_names[tt] ) + tab_step;
            }
            break;
        }
        case FILTERED:
            draw_tab( w, 2, _( "Searched" ), true );
            break;
        case BATCH:
            draw_tab( w, 2, _( "Batch" ), true );
            break;
    }

    wrefresh( w );
}

static void draw_recipe_subtabs( WINDOW *w, std::string tab, std::string subtab, TAB_MODE mode )
{
    werase( w );
    int width = getmaxx( w );
    for( int i = 0; i < width; i++ ) {
        if( i == 0 ) {
            mvwputch( w, 2, i, BORDER_COLOR, LINE_XXXO );
        } else if( i == width ) {
            mvwputch( w, 2, i, BORDER_COLOR, LINE_XOXX );
        } else {
            mvwputch( w, 2, i, BORDER_COLOR, LINE_OXOX );
        }
    }

    for( int i = 0; i < 3; i++ ) {
        mvwputch( w, i,  0, BORDER_COLOR, LINE_XOXO ); // |^
        mvwputch( w, i, width - 1, BORDER_COLOR,  LINE_XOXO ); // ^|
    }

    switch( mode ) {
        case NORMAL: {
            int pos_x = 2;//draw the tabs on each other
            int tab_step = 3;//step between tabs, two for tabs border
            for( const auto stt : craft_subcat_list[tab] ) {
                draw_subtab( w, pos_x, normalized_names[stt], subtab == stt );
                pos_x += utf8_width( normalized_names[stt] ) + tab_step;
            }
            break;
        }
        case FILTERED:
        case BATCH:
            werase( w );
            for( int i = 0; i < 3; i++ ) {
                mvwputch( w, i,  0, BORDER_COLOR, LINE_XOXO ); // |^
                mvwputch( w, i, width - 1, BORDER_COLOR,  LINE_XOXO ); // ^|
            }
            break;
    }

    wrefresh( w );
}

int recipe::print_items( WINDOW *w, int ypos, int xpos, nc_color col, int batch ) const
{
    if( !has_byproducts() ) {
        return 0;
    }

    const int oldy = ypos;

    mvwprintz( w, ypos++, xpos, col, _( "Byproducts:" ) );
    for( const auto &bp : byproducts ) {
        mvwprintz( w, ypos++, xpos, col, _( "> %d %s" ), bp.second * batch,
                   item::nname( bp.first ).c_str() );
    }

    return ypos - oldy;
}

int recipe::print_time( WINDOW *w, int ypos, int xpos, int width,
                        nc_color col, int batch ) const
{
    const int turns = batch_time( batch ) / 100;
    std::string text;
    if( turns < MINUTES( 1 ) ) {
        const int seconds = std::max( 1, turns * 6 );
        text = string_format( ngettext( "%d second", "%d seconds", seconds ), seconds );
    } else {
        const int minutes = ( turns % HOURS( 1 ) ) / MINUTES( 1 );
        const int hours = turns / HOURS( 1 );
        if( hours == 0 ) {
            text = string_format( ngettext( "%d minute", "%d minutes", minutes ), minutes );
        } else if( minutes == 0 ) {
            text = string_format( ngettext( "%d hour", "%d hours", hours ), hours );
        } else {
            const std::string h = string_format( ngettext( "%d hour", "%d hours", hours ), hours );
            const std::string m = string_format( ngettext( "%d minute", "%d minutes", minutes ), minutes );
            //~ A time duration: first is hours, second is minutes, e.g. "4 hours" "6 minutes"
            text = string_format( _( "%1$s and %2$s" ), h.c_str(), m.c_str() );
        }
    }
    text = string_format( _( "Time to complete: %s" ), text.c_str() );
    return fold_and_print( w, ypos, xpos, width, col, text );
}

// ui.cpp
extern bool lcmatch( const std::string &str, const std::string &findstr );

template<typename T>
bool lcmatch_any( const std::vector< std::vector<T> > &list_of_list, const std::string &filter )
{
    for( auto &list : list_of_list ) {
        for( auto &comp : list ) {
            if( lcmatch( item::nname( comp.type ), filter ) ) {
                return true;
            }
        }
    }
    return false;
}
