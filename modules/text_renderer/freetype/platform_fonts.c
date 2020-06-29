/*****************************************************************************
 * freetype.c : Put text on the video, using freetype2
 *****************************************************************************
 * Copyright (C) 2002 - 2015 VLC authors and VideoLAN
 *
 * Authors: Sigmund Augdal Helberg <dnumgis@videolan.org>
 *          Gildas Bazin <gbazin@videolan.org>
 *          Bernie Purcell <bitmap@videolan.org>
 *          Jean-Baptiste Kempf <jb@videolan.org>
 *          Felix Paul Kühne <fkuehne@videolan.org>
 *          Salah-Eddin Shaban <salshaaban@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation, Inc.,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/

/** \ingroup freetype_fonts
 * @{
 * \file
 * Platform-independent font management
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_filter.h>                                      /* filter_sys_t */
#include <vlc_text_style.h>                                  /* text_style_t */
#include <vlc_input.h>                             /* vlc_input_attachment_* */
#include <ctype.h>

#include "platform_fonts.h"
#include "freetype.h"

static FT_Face LoadFace( filter_t *p_filter, const char *psz_fontfile, int i_idx,
                  const text_style_t *p_style )
{
    filter_sys_t *p_sys = p_filter->p_sys;
    char *psz_key = NULL;

    int i_font_size  = ConvertToLiveSize( p_filter, p_style );
    int i_font_width = i_font_size;
    if( p_style->i_style_flags & STYLE_HALFWIDTH )
        i_font_width /= 2;
    else if( p_style->i_style_flags & STYLE_DOUBLEWIDTH )
        i_font_width *= 2;

    if( asprintf( &psz_key, "%s - %d - %d - %d",
                  psz_fontfile, i_idx,
                  i_font_size, i_font_width ) < 0 )
        return NULL;

    FT_Face p_face = vlc_dictionary_value_for_key( &p_sys->face_map, psz_key );
    if( p_face != NULL )
        goto done;

    if( psz_fontfile[0] == ':' && psz_fontfile[1] == '/' )
    {
        int i_attach = atoi( psz_fontfile + 2 );
        if( i_attach < 0 || i_attach >= p_sys->i_font_attachments )
            msg_Err( p_filter, "LoadFace: Invalid font attachment index" );
        else
        {
            input_attachment_t *p_attach = p_sys->pp_font_attachments[ i_attach ];
            if( FT_New_Memory_Face( p_sys->p_library, p_attach->p_data,
                                    p_attach->i_data, i_idx, &p_face ) )
                msg_Err( p_filter, "LoadFace: Error creating face for %s", psz_key );
        }
    }

#if defined( _WIN32 )
    else if( !memcmp( psz_fontfile, ":dw/", 4 ) )
    {
        int i_index = atoi( psz_fontfile + 4 );
        FT_Stream p_stream;
        if( DWrite_GetFontStream( p_filter, i_index, &p_stream ) != VLC_SUCCESS )
            msg_Err( p_filter, "LoadFace: Invalid font stream index" );
        else
        {
            FT_Open_Args args = {0};
            args.flags = FT_OPEN_STREAM;
            args.stream = p_stream;
            if( FT_Open_Face( p_sys->p_library, &args, i_idx, &p_face ) )
                msg_Err( p_filter, "LoadFace: Error creating face for %s", psz_key );
        }
    }
#endif

    else
        if( FT_New_Face( p_sys->p_library, psz_fontfile, i_idx, &p_face ) )
            msg_Err( p_filter, "LoadFace: Error creating face for %s", psz_key );

    if( !p_face )
        goto done;

    if( FT_Select_Charmap( p_face, ft_encoding_unicode ) )
    {
        /* We've loaded a font face which is unhelpful for actually
         * rendering text - fallback to the default one.
         */
        msg_Err( p_filter, "LoadFace: Error selecting charmap for %s", psz_key );
        FT_Done_Face( p_face );
        p_face = NULL;
        goto done;
    }

    if( FT_Set_Pixel_Sizes( p_face, i_font_width, i_font_size ) )
    {
        msg_Err( p_filter,
                 "LoadFace: Failed to set font size for %s", psz_key );
        FT_Done_Face( p_face );
        p_face = NULL;
        goto done;
    }

    vlc_dictionary_insert( &p_sys->face_map, psz_key, p_face );

done:
    free( psz_key );
    return p_face;
}

FT_Face GetFace( filter_t *p_filter, vlc_font_t *p_font, uni_char_t codepoint )
{
    filter_sys_t *p_sys = p_filter->p_sys;

    if( !p_font->p_face )
        p_font->p_face = LoadFace( p_filter, p_font->psz_fontfile,
                                   p_font->i_index,
                                   p_sys->p_default_style );

    if( p_font->p_face &&
        FT_Get_Char_Index( p_font->p_face, codepoint ) )
        return p_font->p_face;
    else
        return NULL;
}

/**
 * Select the best font from the list of vlc_font_t's of the given family.
 * If a family does not have the exact requested style, the nearest one will be returned.
 * Like when an italic font is requested from a family which has only a regular font. In this
 * case the regular font will be returned and FreeType will do synthetic styling on it.
 *
 * Not all fonts of a family support the same scripts. As an example, when an italic font
 * containing an Arabic codepoint is requested from the Arial family, the regular font will
 * be returned, because the italic font of Arial has no Arabic support.
 */
static vlc_font_t *GetBestFont( filter_t *p_filter, const vlc_family_t *p_family,
                                bool b_bold, bool b_italic, uni_char_t codepoint )
{
    int i_best_score = 0;
    vlc_font_t *p_best_font = p_family->p_fonts;

    for( vlc_font_t *p_font = p_family->p_fonts; p_font; p_font = p_font->p_next )
    {
        int i_score = 0;

        if( codepoint && GetFace( p_filter, p_font, codepoint ) )
            i_score += 1000;

        if( !!p_font->b_bold == !!b_bold )
            i_score += 100;
        if( !!p_font->b_italic == !!b_italic )
            i_score += 10;

        if( i_score > i_best_score )
        {
            p_best_font = p_font;
            i_best_score = i_score;
        }
    }

    return p_best_font;
}

vlc_family_t *SearchFallbacks( filter_t *p_filter, vlc_family_t *p_fallbacks,
                                      uni_char_t codepoint )
{
    filter_sys_t *p_sys = p_filter->p_sys;
    vlc_family_t *p_family = NULL;

    for( vlc_family_t *p_fallback = p_fallbacks; p_fallback;
         p_fallback = p_fallback->p_next )
    {
        if( !p_fallback->p_fonts )
        {
            const vlc_family_t *p_temp =
                    p_sys->pf_get_family( p_filter, p_fallback->psz_name );
            if( !p_temp || !p_temp->p_fonts )
                continue;
            p_fallback->p_fonts = p_temp->p_fonts;
        }

        if( !GetFace( p_filter, p_fallback->p_fonts, codepoint ) )
            continue;

        p_family = p_fallback;
        break;
    }

    return p_family;
}

static vlc_family_t *SearchFontByFamilyName( filter_t *p_filter,
                                             vlc_family_t *p_list,
                                             const char *psz_familyname,
                                             uni_char_t codepoint )
{
    for( vlc_family_t *p = p_list; p; p = p->p_next )
    {
        if( !strcasecmp( p->psz_name, psz_familyname ) &&
            p->p_fonts &&
            GetFace( p_filter, p->p_fonts, codepoint ) )
            return p;
    }
    return NULL;
}

static inline void AppendFont( vlc_font_t **pp_list, vlc_font_t *p_font )
{
    while( *pp_list )
        pp_list = &( *pp_list )->p_next;

    *pp_list = p_font;
}

static inline void AppendFamily( vlc_family_t **pp_list, vlc_family_t *p_family )
{
    while( *pp_list )
        pp_list = &( *pp_list )->p_next;

    *pp_list = p_family;
}

vlc_family_t *NewFamily( filter_t *p_filter, const char *psz_family,
                         vlc_family_t **pp_list, vlc_dictionary_t *p_dict,
                         const char *psz_key )
{
    filter_sys_t *p_sys = p_filter->p_sys;

    vlc_family_t *p_family = calloc( 1, sizeof( *p_family ) );
    if( unlikely(!p_family) )
        return NULL;

    char *psz_name;
    if( psz_family && *psz_family )
        psz_name = ToLower( psz_family );
    else
        if( asprintf( &psz_name, FB_NAME"-%04d",
                      p_sys->i_fallback_counter++ ) < 0 )
            psz_name = NULL;

    char *psz_lc = NULL;
    if( likely( psz_name ) )
    {
        if( !psz_key )
            psz_lc = strdup( psz_name );
        else
            psz_lc = ToLower( psz_key );
    }

    if( unlikely( !p_family || !psz_name || !psz_lc ) )
    {
        free( p_family );
        free( psz_name );
        free( psz_lc );
        return NULL;
    }

    p_family->psz_name = psz_name;

    if( pp_list )
        AppendFamily( pp_list, p_family );

    if( p_dict )
    {
        vlc_family_t *p_root = vlc_dictionary_value_for_key( p_dict, psz_lc );
        if( p_root )
            AppendFamily( &p_root, p_family );
        else
            vlc_dictionary_insert( p_dict, psz_lc, p_family );
    }

    free( psz_lc );
    return p_family;
}

vlc_font_t *NewFont( char *psz_fontfile, int i_index,
                     bool b_bold, bool b_italic,
                     vlc_family_t *p_parent )
{
    vlc_font_t *p_font = calloc( 1, sizeof( *p_font ) );

    if( unlikely( !p_font ) )
    {
        free( psz_fontfile );
        return NULL;
    }

    p_font->psz_fontfile = psz_fontfile;
    p_font->i_index = i_index;
    p_font->b_bold = b_bold;
    p_font->b_italic = b_italic;

    if( p_parent )
    {
        /* Keep regular faces first */
        if( p_parent->p_fonts
         && ( p_parent->p_fonts->b_bold || p_parent->p_fonts->b_italic )
         && !b_bold && !b_italic )
        {
            p_font->p_next = p_parent->p_fonts;
            p_parent->p_fonts = p_font;
        }
        else
            AppendFont( &p_parent->p_fonts, p_font );
    }

    return p_font;
}

void FreeFamiliesAndFonts( vlc_family_t *p_family )
{
    if( p_family->p_next )
        FreeFamiliesAndFonts( p_family->p_next );

    for( vlc_font_t *p_font = p_family->p_fonts; p_font; )
    {
        vlc_font_t *p_temp = p_font->p_next;
        free( p_font->psz_fontfile );
        free( p_font );
        p_font = p_temp;
    }

    free( p_family->psz_name );
    free( p_family );
}

void FreeFamilies( void *p_families, void *p_obj )
{
    vlc_family_t *p_family = ( vlc_family_t * ) p_families;

    if( p_family->p_next )
        FreeFamilies( p_family->p_next, p_obj );

    free( p_family->psz_name );
    free( p_family );
}

vlc_family_t *InitDefaultList( filter_t *p_filter, const char *const *ppsz_default,
                               int i_size )
{

    vlc_family_t  *p_default  = NULL;
    filter_sys_t  *p_sys = p_filter->p_sys;

    for( int i = 0; i < i_size; ++i )
    {
        const vlc_family_t *p_family =
                p_sys->pf_get_family( p_filter, ppsz_default[ i ] );

        if( p_family )
        {
            vlc_family_t *p_temp =
                NewFamily( p_filter, ppsz_default[ i ], &p_default, NULL, NULL );

            if( unlikely( !p_temp ) )
                goto error;

            p_temp->p_fonts = p_family->p_fonts;
        }
    }

    if( p_default )
        vlc_dictionary_insert( &p_sys->fallback_map, FB_LIST_DEFAULT, p_default );

    return p_default;

error:
    if( p_default ) FreeFamilies( p_default, NULL );
    return NULL;
}

#ifdef DEBUG_PLATFORM_FONTS
void DumpFamily( filter_t *p_filter, const vlc_family_t *p_family,
                 bool b_dump_fonts, int i_max_families )
{

    if( i_max_families < 0 )
        i_max_families = INT_MAX;

    for( int i = 0; p_family && i < i_max_families ; p_family = p_family->p_next, ++i )
    {
        msg_Dbg( p_filter, "\t[%p] %s", (void *)p_family, p_family->psz_name );

        if( b_dump_fonts )
        {
            for( vlc_font_t *p_font = p_family->p_fonts; p_font; p_font = p_font->p_next )
            {
                const char *psz_style = NULL;
                if( !p_font->b_bold && !p_font->b_italic )
                    psz_style = "Regular";
                else if( p_font->b_bold && !p_font->b_italic )
                    psz_style = "Bold";
                else if( !p_font->b_bold && p_font->b_italic )
                    psz_style = "Italic";
                else if( p_font->b_bold && p_font->b_italic )
                    psz_style = "Bold Italic";

                msg_Dbg( p_filter, "\t\t[%p] (%s): %s - %d", (void *)p_font,
                         psz_style, p_font->psz_fontfile, p_font->i_index );
            }

        }
    }
}

void DumpDictionary( filter_t *p_filter, const vlc_dictionary_t *p_dict,
                     bool b_dump_fonts, int i_max_families )
{
    char **ppsz_keys = vlc_dictionary_all_keys( p_dict );

    if( unlikely( !ppsz_keys ) )
        return;

    for( int i = 0; ppsz_keys[ i ]; ++i )
    {
        vlc_family_t *p_family = vlc_dictionary_value_for_key( p_dict, ppsz_keys[ i ] );
        msg_Dbg( p_filter, "Key: %s", ppsz_keys[ i ] );
        if( p_family )
            DumpFamily( p_filter, p_family, b_dump_fonts, i_max_families );
        free( ppsz_keys[ i ] );
    }
    free( ppsz_keys );
}
#endif

char* ToLower( const char *psz_src )
{
    int i_size = strlen( psz_src ) + 1;
    char *psz_buffer = malloc( i_size );
    if( unlikely( !psz_buffer ) )
        return NULL;

    for( int i = 0; i < i_size; ++i )
        psz_buffer[ i ] = tolower( psz_src[ i ] );

    return psz_buffer;
}

/* Face loading */
int ConvertToLiveSize( filter_t *p_filter, const text_style_t *p_style )
{
    filter_sys_t *p_sys = p_filter->p_sys;

    int i_font_size = STYLE_DEFAULT_FONT_SIZE;
    if( p_style->i_font_size )
    {
        i_font_size = p_style->i_font_size;
    }
    else if ( p_style->f_font_relsize )
    {
        i_font_size = (int) p_filter->fmt_out.video.i_height * p_style->f_font_relsize / 100;
    }

    if( p_sys->i_scale != 100 )
        i_font_size = i_font_size * p_sys->i_scale / 100;

    return i_font_size;
}

static void TrimWhiteSpace( const char **pp, const char **ppend )
{
    for( ; *pp < *ppend ; ++(*pp) )
        if( **pp != ' ' && **pp != '\t' )
            break;
    for( ; *ppend > *pp ; --(*ppend) )
        if( *((*ppend)- 1) != ' ' && *((*ppend)- 1) != '\t' )
            break;
}

static void AddSingleFamily( const char *psz_start,
                             const char *psz_end,
                             fontfamilies_t *families )
{
    TrimWhiteSpace( &psz_start, &psz_end );
    /* basic unquote */
    if( psz_end > psz_start &&
        *psz_start == '"' && psz_end[-1] == '"' )
    {
        ++psz_start;
        --psz_end;
    }
    if( psz_end > psz_start )
    {
        char *psz = strndup( psz_start, psz_end - psz_start );
        if( psz )
            vlc_vector_push( families, psz );
    }
}

static void SplitIntoSingleFamily( const char *psz_spec, fontfamilies_t *families )
{
    if( !psz_spec )
        return;

    char *dup = strdup( psz_spec );
    char *p_savetpr;
    for ( const char *psz = strtok_r( dup, ",", &p_savetpr );
          psz != NULL;
          psz = strtok_r( NULL, ",", &p_savetpr ) )
    {
        AddSingleFamily( psz, psz + strlen( psz ), families );
    }
    free( dup );
}

static char* SelectFontWithFamilyFallback( filter_t *p_filter,
                                    const fontfamilies_t *families,
                                    const text_style_t *p_style,
                                    int *pi_idx, uni_char_t codepoint )
{
    filter_sys_t *p_sys = p_filter->p_sys;

    const bool b_bold = p_style->i_style_flags & STYLE_BOLD;
    const bool b_italic = p_style->i_style_flags & STYLE_ITALIC;
    const vlc_family_t *p_family = NULL;

    if( codepoint )
    {
        vlc_family_t *p_fallbacks;
        const char *psz_name;
        /*
         * Try regular face of the same family first.
         * It usually has the best coverage.
         */
        vlc_vector_foreach( psz_name, families )
        {
            p_fallbacks = vlc_dictionary_value_for_key( &p_sys->fallback_map,
                                                        FB_LIST_ATTACHMENTS );
            if( p_fallbacks )
            {
                p_family = SearchFontByFamilyName( p_filter, p_fallbacks,
                                                   psz_name, codepoint );
                if( p_family )
                    break;
            }

            p_family = p_sys->pf_get_family( p_filter, psz_name );
            if( p_family && p_family->p_fonts &&
                GetFace( p_filter, p_family->p_fonts, codepoint ) )
            {
                break;
            }

            p_family = NULL;
        }

        /* Try font attachments if not available locally */
        if( !p_family )
        {
            p_fallbacks = vlc_dictionary_value_for_key( &p_sys->fallback_map,
                                                        FB_LIST_ATTACHMENTS );
            if( p_fallbacks )
                p_family = SearchFallbacks( p_filter, p_fallbacks, codepoint );
        }

        /* Try system fallbacks */
        if( !p_family && p_sys->pf_get_fallbacks )
        {
            vlc_vector_foreach( psz_name, families )
            {
                p_fallbacks = p_sys->pf_get_fallbacks( p_filter, psz_name, codepoint );
                if( p_fallbacks )
                {
                    p_family = SearchFallbacks( p_filter, p_fallbacks, codepoint );
                    if( p_family && p_family->p_fonts )
                        break;
                }
                p_family = NULL;
            }
        }

        /* Try the default fallback list, if any */
        if( !p_family )
        {
            p_fallbacks = vlc_dictionary_value_for_key( &p_sys->fallback_map,
                                                        FB_LIST_DEFAULT );
            if( p_fallbacks )
                p_family = SearchFallbacks( p_filter, p_fallbacks, codepoint );
        }

        if( !p_family )
            return NULL;
    }

    if( !p_family || !p_family->p_fonts )
        p_family = p_sys->pf_get_family( p_filter, DEFAULT_FAMILY );

    vlc_font_t *p_font;
    if( p_family && ( p_font = GetBestFont( p_filter, p_family, b_bold,
                                            b_italic, codepoint ) ) )
    {
        *pi_idx = p_font->i_index;
        return strdup( p_font->psz_fontfile );
    }

    return NULL;
}

FT_Face SelectAndLoadFace( filter_t *p_filter, const text_style_t *p_style,
                           uni_char_t codepoint )
{
    const char *psz_fontname = (p_style->i_style_flags & STYLE_MONOSPACED)
                               ? p_style->psz_monofontname : p_style->psz_fontname;

    fontfamilies_t families;
    vlc_vector_init( &families );
    SplitIntoSingleFamily( psz_fontname, &families );
    if( families.size == 0 )
    {
        vlc_vector_clear( &families );
        return NULL;
    }

    FT_Face p_face = NULL;

    int  i_idx = 0;
    char *psz_fontfile =
            SelectFontWithFamilyFallback( p_filter, &families, p_style,
                                          &i_idx, codepoint );
    if( psz_fontfile && *psz_fontfile != '\0' )
    {
        p_face = LoadFace( p_filter, psz_fontfile, i_idx, p_style );
    }
    else
    {
        msg_Warn( p_filter,
                  "SelectAndLoadFace: no font found for family: %s, codepoint: 0x%x",
                  psz_fontname, codepoint );
    }

    char *psz_temp;
    vlc_vector_foreach( psz_temp, &families )
        free( psz_temp );
    vlc_vector_clear( &families );
    free( psz_fontfile );
    return p_face;
}

#ifndef HAVE_GET_FONT_BY_FAMILY_NAME
const vlc_family_t * StaticMap_GetFamily( filter_t *p_filter,
                                          const char *psz_family )
{
    filter_sys_t *p_sys = p_filter->p_sys;
    char *psz_lc = ToLower( psz_family );

    if( unlikely( !psz_lc ) )
        return NULL;

    vlc_family_t *p_family =
        vlc_dictionary_value_for_key( &p_sys->family_map, psz_lc );
    if( p_family )
    {
        free( psz_lc );
        return p_family;
    }

    const char *psz_file = NULL;
    if( !strcasecmp( psz_family, DEFAULT_FAMILY ) )
    {
        psz_file = p_sys->psz_fontfile ? p_sys->psz_fontfile
                                       : DEFAULT_FONT_FILE;
    }
    else if( !strcasecmp( psz_family, DEFAULT_MONOSPACE_FAMILY ) )
    {
        psz_file = p_sys->psz_monofontfile ? p_sys->psz_monofontfile
                                           : DEFAULT_MONOSPACE_FONT_FILE;
    }

    if( !psz_file )
    {
        free( psz_lc );
        return NULL;
    }

    /* Create new entry */
    p_family = NewFamily( p_filter, psz_lc, &p_sys->p_families,
                          &p_sys->family_map, psz_lc );

    free( psz_lc );

    if( unlikely( !p_family ) )
        return NULL;

    char *psz_font_file = MakeFilePath( p_filter, psz_file );
    if( psz_font_file )
        NewFont( psz_font_file, 0, false, false, p_family );

    return p_family;
}
#endif

#if !defined(_WIN32) || VLC_WINSTORE_APP

char * MakeFilePath( filter_t *p_filter, const char *psz_filename )
{
    VLC_UNUSED(p_filter);

    if( !psz_filename )
        return NULL;

    /* Handle the case where the user redefined *_FILE using FQN */
    if( psz_filename[0] == DIR_SEP_CHAR )
        return strdup( psz_filename );

    char *psz_filepath;
    if( asprintf( &psz_filepath, "%s" DIR_SEP "%s",
                  SYSTEM_FONT_PATH, psz_filename ) == -1 )
        psz_filepath = NULL;

    return psz_filepath;
}
#endif
