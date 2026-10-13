// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_DataConsent_h
#define PICopilot_DataConsent_h

#include <pcl/String.h>

namespace pcl { namespace DataConsent {

/*
 * CPD rule 6.2: nothing is sent to Anthropic until the user has agreed, in a
 * dialog that names Anthropic and lists exactly what can be sent. The
 * agreement is stored with the PCL Settings API (PICopilot/AnthropicConsent*)
 * and can be withdrawn in ⚙. It is versioned: when what PI Copilot sends
 * changes, kVersion goes up and the user is asked again.
 *
 * Enforcement is at the one place every request is built
 * (AnthropicRequest's constructor, root thread): without the agreement a
 * request is never configured and Perform() returns a Consent error without
 * touching the network -- chat turns and journey write-ups alike.
 */

constexpr int kVersion = 1;

// Root thread (PCL Settings).
bool   Granted();
String GrantedAt();   // ISO 8601 date of the agreement; "" when not granted
void   Grant();
void   Revoke();

// The dialog text (rich text): who receives the data, exactly what can be
// sent, what never is, the privacy policy link, and how to withdraw.
String ConsentHtml( bool question = true );   // question: ends with "Do you agree ...?"

// Why a request was not sent (for errors and the chat log).
String NotGrantedText();

// The modal agreement dialog: Yes = "I agree", No = don't send (default and
// Esc). Grants on Yes. Root thread. True when the user agreed.
bool Ask();

} } // namespace pcl::DataConsent

#endif // PICopilot_DataConsent_h
