// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "HistoryBudget.h"
#include "Utf8.h"

#include <string>

namespace pcl
{

const char* const kPICopilotTrimNote =
   "[Earlier parts of this conversation were removed to keep it within PI Copilot's history budget.]";

namespace
{

bool TypeIs( const nlohmann::json& v, const char* type )
{
   if ( !v.is_object() )
      return false;
   const auto t = v.find( "type" );
   return t != v.end() && t->is_string() && t->get_ref<const std::string&>() == type;
}

size_type TextBytes( const nlohmann::json& v, size_type& images )
{
   if ( v.is_string() )
      return v.get_ref<const std::string&>().size();
   if ( v.is_object() )
   {
      if ( TypeIs( v, "image" ) )
      {
         ++images;
         return 0;
      }
      size_type n = 0;
      for ( auto it = v.begin(); it != v.end(); ++it )
         n += it.key().size() + TextBytes( it.value(), images );
      return n;
   }
   if ( v.is_array() )
   {
      size_type n = 0;
      for ( const nlohmann::json& e : v )
         n += TextBytes( e, images );
      return n;
   }
   return 8;   // number / bool / null
}

} // namespace

size_type EstimateMessageTokens( const AnthropicMessage& m )
{
   size_type images = 0, bytes = 0;
   if ( !m.blocks.is_null() )
      bytes = TextBytes( m.blocks, images );
   else
   {
      bytes = U8( m.content ).size();
      if ( !m.imageJpegBase64.IsEmpty() )
         ++images;
   }
   return bytes/3 + images*PICopilotImageTokenEstimate + 8;
}

size_type EstimateHistoryTokens( const Array<AnthropicMessage>& h, size_type from )
{
   size_type n = 0;
   for ( size_type i = from; i < h.Length(); ++i )
      n += EstimateMessageTokens( h[i] );
   return n;
}

bool IsFreshUserTurn( const AnthropicMessage& m )
{
   if ( m.role != "user" )
      return false;
   if ( !m.blocks.is_array() || m.blocks.empty() )
      return true;
   return !TypeIs( m.blocks[0], "tool_result" );
}

size_type TrimHistoryToBudget( Array<AnthropicMessage>& h, size_type budget, size_type target )
{
   if ( h.Length() < 3 )
      return 0;
   // suffix[i] = EstimateHistoryTokens( h, i ), computed once (linear).
   Array<size_type> suffix( h.Length() + 1, size_type( 0 ) );
   for ( size_type i = h.Length(); i-- > 0; )
      suffix[i] = suffix[i+1] + EstimateMessageTokens( h[i] );
   if ( suffix[0] <= budget )
      return 0;
   size_type lastFresh = 0;
   for ( size_type i = h.Length(); i-- > 0; )
      if ( IsFreshUserTurn( h[i] ) )
      {
         lastFresh = i;
         break;
      }
   if ( lastFresh == 0 )
      return 0;   // the whole history is the current exchange: nothing may be cut
   size_type cut = lastFresh;
   for ( size_type i = 2; i < lastFresh; ++i )
      if ( IsFreshUserTurn( h[i] ) && suffix[i] <= target )
      {
         cut = i;
         break;
      }
   // Build the new first message's blocks before removing anything, so a
   // throw while building them leaves h untouched.
   const AnthropicMessage& first = h[cut];
   nlohmann::json blocks = nlohmann::json::array();
   blocks.push_back( { { "type", "text" }, { "text", kPICopilotTrimNote } } );
   if ( first.blocks.is_array() )
      for ( const nlohmann::json& b : first.blocks )
         blocks.push_back( b );
   else
   {
      if ( !first.imageJpegBase64.IsEmpty() )
         blocks.push_back( JpegImageBlock( first.imageJpegBase64 ) );
      if ( !first.content.IsEmpty() )
         blocks.push_back( { { "type", "text" }, { "text", U8( first.content ) } } );
   }

   h.Remove( h.Begin(), h.At( cut ) );
   AnthropicMessage& newFirst = h[0];
   newFirst.blocks = std::move( blocks );
   newFirst.content.Clear();
   newFirst.imageJpegBase64.Clear();
   return cut;
}

} // namespace pcl
