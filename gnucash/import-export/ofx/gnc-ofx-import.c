/*******************************************************************\
 * This program is free software; you can redistribute it and/or    *
 * modify it under the terms of the GNU General Public License as   *
 * published by the Free Software Foundation; either version 2 of   *
 * the License, or (at your option) any later version.              *
 *                                                                  *
 * This program is distributed in the hope that it will be useful,  *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of   *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the    *
 * GNU General Public License for more details.                     *
 *                                                                  *
 * You should have received a copy of the GNU General Public License*
 * along with this program; if not, contact:                        *
 *                                                                  *
 * Free Software Foundation           Voice:  +1-617-542-5942       *
 * 51 Franklin Street, Fifth Floor    Fax:    +1-617-542-2652       *
 * Boston, MA  02110-1301,  USA       gnu@gnu.org                   *
\********************************************************************/
/** @addtogroup Import_Export
    @{ */
/** @internal
     @file gnc-ofx-import.c
     @brief Ofx import module code
     @author Copyright (c) 2002 Benoit Grégoire <bock@step.polymtl.ca>
 */
#include <config.h>

#include <gtk/gtk.h>
#include <glib/gi18n.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <math.h>

#include <libofx/libofx.h>
#include "import-account-matcher.h"
#include "import-commodity-matcher.h"
#include "import-utilities.h"
#include "import-main-matcher.h"

#include "Account.h"
#include "Transaction.h"
#include "engine-helpers.h"
#include "gnc-ofx-import.h"
#include "gnc-file.h"
#include "gnc-engine.h"
#include "gnc-ui-util.h"
#include "gnc-glib-utils.h"
#include "gnc-prefs.h"
#include "gnc-ui.h"
#include "dialog-account.h"
#include "dialog-utils.h"
#include "window-reconcile.h"

#define GNC_PREFS_GROUP "dialogs.import.ofx"
#define GNC_PREF_AUTO_COMMODITY "auto-create-commodity"

static QofLogModule log_module = GNC_MOD_IMPORT;

/********************************************************************\
 * gnc_file_ofx_import
 * Entry point
\********************************************************************/

static gboolean auto_create_commodity = FALSE;
static Account *ofx_parent_account = NULL;
// Structure we use to gather information about statement balance/account etc.
typedef struct _ofx_info
{
    GtkWindow* parent;
    GNCImportMainMatcher *gnc_ofx_importer_gui;
    Account *last_import_account;
    Account *last_investment_account;
    Account *last_income_account;
    gint num_trans_processed;               // Number of transactions processed
    struct OfxStatementData* statement;     // Statement, if any
    gboolean run_reconcile;                 // If TRUE the reconcile window is opened after matching.
    GSList* file_list;                      // List of OFX files to import
    GList* trans_list;                      // We store the processed ofx transactions here
    gint response;                          // Response sent by the match gui
} ofx_info ;

static void runMatcher(ofx_info* info, char * selected_filename, gboolean go_to_next_file);

/*
int ofx_proc_status_cb(struct OfxStatusData data)
{
  return 0;
}
*/

static const char *PROP_OFX_INCOME_ACCOUNT = "ofx-income-account";

static Account*
get_associated_income_account(const Account* investment_account)
{
    GncGUID *income_guid = NULL;
    Account *acct = NULL;
    g_assert(investment_account);
    qof_instance_get (QOF_INSTANCE (investment_account),
                      PROP_OFX_INCOME_ACCOUNT, &income_guid,
                      NULL);
    acct = xaccAccountLookup (income_guid,
                              gnc_account_get_book(investment_account));
    guid_free (income_guid);
    return acct;
}

static void
set_associated_income_account(Account* investment_account,
                              const Account *income_account)
{
    const GncGUID * income_acc_guid;

    g_assert(investment_account);
    g_assert(income_account);

    income_acc_guid = xaccAccountGetGUID(income_account);
    xaccAccountBeginEdit(investment_account);
    qof_instance_set (QOF_INSTANCE (investment_account),
		      PROP_OFX_INCOME_ACCOUNT, income_acc_guid,
		      NULL);
    xaccAccountCommitEdit(investment_account);
}

int ofx_proc_statement_cb (struct OfxStatementData data, void * statement_user_data);
int ofx_proc_security_cb (const struct OfxSecurityData data, void * security_user_data);
int ofx_proc_transaction_cb (struct OfxTransactionData data, void *user_data);
int ofx_proc_account_cb (struct OfxAccountData data, void * account_user_data);
static double ofx_get_investment_amount (const struct OfxTransactionData* data);

static const gchar *gnc_ofx_ttype_to_string(TransactionType t)
{
    switch (t)
    {
    case OFX_CREDIT:
        return "Generic credit";
    case OFX_DEBIT:
        return "Generic debit";
    case OFX_INT:
        return "Interest earned or paid (Note: Depends on signage of amount)";
    case OFX_DIV:
        return "Dividend";
    case OFX_FEE:
        return "FI fee";
    case OFX_SRVCHG:
        return "Service charge";
    case OFX_DEP:
        return "Deposit";
    case OFX_ATM:
        return "ATM debit or credit (Note: Depends on signage of amount)";
    case OFX_POS:
        return "Point of sale debit or credit (Note: Depends on signage of amount)";
    case OFX_XFER:
        return "Transfer";
    case OFX_CHECK:
        return "Check";
    case OFX_PAYMENT:
        return "Electronic payment";
    case OFX_CASH:
        return "Cash withdrawal";
    case OFX_DIRECTDEP:
        return "Direct deposit";
    case OFX_DIRECTDEBIT:
        return "Merchant initiated debit";
    case OFX_REPEATPMT:
        return "Repeating payment/standing order";
    case OFX_OTHER:
        return "Other";
    default:
        return "Unknown transaction type";
    }
}

static const gchar *gnc_ofx_invttype_to_str(InvTransactionType t)
{
    switch (t)
    {
    case OFX_BUYDEBT:
        return "BUYDEBT (Buy debt security)";
    case OFX_BUYMF:
        return "BUYMF (Buy mutual fund)";
    case OFX_BUYOPT:
        return "BUYOPT (Buy option)";
    case OFX_BUYOTHER:
        return "BUYOTHER (Buy other security type)";
    case OFX_BUYSTOCK:
        return "BUYSTOCK (Buy stock))";
    case OFX_CLOSUREOPT:
        return "CLOSUREOPT (Close a position for an option)";
    case OFX_INCOME:
        return "INCOME (Investment income is realized as cash into the investment account)";
    case OFX_INVEXPENSE:
        return "INVEXPENSE (Misc investment expense that is associated with a specific security)";
    case OFX_JRNLFUND:
        return "JRNLFUND (Journaling cash holdings between subaccounts within the same investment account)";
    case OFX_MARGININTEREST:
        return "MARGININTEREST (Margin interest expense)";
    case OFX_REINVEST:
        return "REINVEST (Reinvestment of income)";
    case OFX_RETOFCAP:
        return "RETOFCAP (Return of capital)";
    case OFX_SELLDEBT:
        return "SELLDEBT (Sell debt security.  Used when debt is sold, called, or reached maturity)";
    case OFX_SELLMF:
        return "SELLMF (Sell mutual fund)";
    case OFX_SELLOPT:
        return "SELLOPT (Sell option)";
    case OFX_SELLOTHER:
        return "SELLOTHER (Sell other type of security)";
    case OFX_SELLSTOCK:
        return "SELLSTOCK (Sell stock)";
    case OFX_SPLIT:
        return "SPLIT (Stock or mutial fund split)";
    case OFX_TRANSFER:
        return "TRANSFER (Transfer holdings in and out of the investment account)";
#ifdef HAVE_LIBOFX_VERSION_0_10
    case OFX_INVBANKTRAN:
         return "Transfer cash in and out of the investment account";
#endif
    default:
        return "ERROR, this investment transaction type is unknown.  This is a bug in ofxdump";
    }

}

static gchar*
sanitize_string (gchar* str)
{
    gchar *inval;
    const int length = -1; /*Assumes str is null-terminated */
    while (!g_utf8_validate (str, length, (const gchar **)(&inval)))
	*inval = '@';
    return str;
}

int ofx_proc_security_cb(const struct OfxSecurityData data, void * security_user_data)
{
    char* cusip = NULL;
    char* default_fullname = NULL;
    char* default_mnemonic = NULL;

    ofx_info* info = (ofx_info*) security_user_data;

    if (data.unique_id_valid)
    {
        cusip = gnc_utf8_strip_invalid_strdup (data.unique_id);
    }
    if (data.secname_valid)
    {
        default_fullname = gnc_utf8_strip_invalid_strdup (data.secname);
    }
    if (data.ticker_valid)
    {
        default_mnemonic = gnc_utf8_strip_invalid_strdup (data.ticker);
    }

    if (auto_create_commodity)
    {
        gnc_commodity *commodity =
            gnc_import_select_commodity(cusip,
                                        FALSE,
                                        default_fullname,
                                        default_mnemonic);

        if (!commodity)
        {
            QofBook *book = gnc_get_current_book();
            gnc_quote_source *source;
            gint source_selection = 0; // FIXME: This is just a wild guess
            char *commodity_namespace = NULL;
            int fraction = 1;

            if (data.unique_id_type_valid)
            {
                commodity_namespace = gnc_utf8_strip_invalid_strdup (data.unique_id_type);
            }

            g_warning("Creating a new commodity, cusip=%s", cusip);
            /* Create the new commodity */
            commodity = gnc_commodity_new(book,
                                          default_fullname,
                                          commodity_namespace,
                                          default_mnemonic,
                                          cusip,
                                          fraction);

            /* Also set a single quote source */
            gnc_commodity_begin_edit(commodity);
            gnc_commodity_user_set_quote_flag (commodity, TRUE);
            source = gnc_quote_source_lookup_by_ti (SOURCE_SINGLE, source_selection);
            gnc_commodity_set_quote_source(commodity, source);
            gnc_commodity_commit_edit(commodity);

            /* Remember the commodity */
            gnc_commodity_table_insert(gnc_get_current_commodities(), commodity);

	    g_free (commodity_namespace);

        }
    }
    else
    {
        gnc_import_select_commodity(cusip,
                                    TRUE,
                                    default_fullname,
                                    default_mnemonic);
    }

    g_free (cusip);
    g_free (default_mnemonic);
    g_free (default_fullname);
    return 0;
}

static void gnc_ofx_set_split_memo(const struct OfxTransactionData* data, Split *split)
{
    g_assert(data);
    g_assert(split);
    /* Also put the ofx transaction name in
     * the splits memo field, or ofx memo if
     * name is unavailable */
    if (data->name_valid)
    {
        xaccSplitSetMemo(split, data->name);
    }
    else if (data->memo_valid)
    {
        xaccSplitSetMemo(split, data->memo);
    }
}
static gnc_numeric gnc_ofx_numeric_from_double(double value, const gnc_commodity *commodity)
{
    return double_to_gnc_numeric (value,
                                  gnc_commodity_get_fraction(commodity),
                                  GNC_HOW_RND_ROUND_HALF_UP);
}
static gnc_numeric gnc_ofx_numeric_from_double_txn(double value, const Transaction* txn)
{
    return gnc_ofx_numeric_from_double(value, xaccTransGetCurrency(txn));
}

/* Opens the dialog to create a new account with given name, commodity, parent, type.
 * Returns the new account, or NULL if it couldn't be created.. */
static Account *gnc_ofx_new_account(GtkWindow* parent,
                                    const char* name,
                                    const gnc_commodity * account_commodity,
                                    Account *parent_account,
                                    GNCAccountType new_account_default_type)
{
    Account *result;
    GList * valid_types = NULL;

    g_assert(name);
    g_assert(account_commodity);
    g_assert(parent_account);

    if (new_account_default_type != ACCT_TYPE_NONE)
    {
        // Passing the types as gpointer
        valid_types =
            g_list_prepend(valid_types,
                           GINT_TO_POINTER(new_account_default_type));
        if (!xaccAccountTypesCompatible(xaccAccountGetType(parent_account), new_account_default_type))
        {
            // Need to add the parent's account type
            valid_types =
                g_list_prepend(valid_types,
                               GINT_TO_POINTER(xaccAccountGetType(parent_account)));
        }
    }
    result = gnc_ui_new_accounts_from_name_with_defaults (parent, name,
                                                          valid_types,
                                                          account_commodity,
                                                          parent_account);
    g_list_free(valid_types);
    return result;
}
/* LibOFX has a daylight time handling bug,
 * https://sourceforge.net/p/libofx/bugs/39/, which causes it to adjust the
 * timestamp for daylight time even when daylight time is not in
 * effect. HAVE_OFX_BUG_39 reflects the result of checking for this bug during
 * configuration, and fix_ofx_bug_39() corrects for it.
 */
static time64
fix_ofx_bug_39 (time64 t)
{
#if HAVE_OFX_BUG_39
    struct tm stm;

#ifdef __FreeBSD__
    time64 now;
    /*
     * FreeBSD has it's own libc implementation which differs from glibc. In particular:
     * There is no daylight global
     * tzname members are set to the string "   " (three spaces) when not explicitly populated
     *
     * To check that the current timezone does not observe DST I check if tzname[1] starts with a space.
     */
    now = gnc_time (NULL);
    gnc_localtime_r(&now, &stm);
    tzset();

    if (tzname[1][0] != ' ' && !stm.tm_isdst)
#else
    gnc_localtime_r(&t, &stm);
    if (daylight && !stm.tm_isdst)
#endif
        t += 3600;
#endif
    return t;
}

int ofx_proc_transaction_cb(struct OfxTransactionData data, void *user_data)
{
    char dest_string[255];
    time64 current_time = gnc_time (NULL);
    Account *account;
    Account *investment_account = NULL;
    Account *income_account = NULL;
    gchar *investment_account_text, *investment_account_onlineid;
    gnc_commodity *currency = NULL;
    gnc_commodity *investment_commodity = NULL;
    gnc_numeric gnc_amount, gnc_units;
    QofBook *book;
    Transaction *transaction;
    Split *split;
    gchar *notes, *tmp;
    ofx_info* info = (ofx_info*) user_data;
    GtkWindow *parent = GTK_WINDOW (info->parent);

    g_assert(info->gnc_ofx_importer_gui);

    if (!data.account_id_valid)
    {
        PERR("account ID for this transaction is unavailable!");
        return 0;
    }
    else
	gnc_utf8_strip_invalid (data.account_id);

    account = gnc_import_select_account(gnc_gen_trans_list_widget(info->gnc_ofx_importer_gui),
                                        data.account_id,
					0, NULL, NULL, ACCT_TYPE_NONE,
					info->last_import_account, NULL);
    if (account == NULL)
    {
        PERR("Unable to find account for id %s", data.account_id);
        return 0;
    }
    info->last_import_account = account;
    /***** Validate the input strings to ensure utf8 *****/
    if (data.name_valid)
        gnc_utf8_strip_invalid(data.name);
    if (data.memo_valid)
        gnc_utf8_strip_invalid(data.memo);
    if (data.check_number_valid)
        gnc_utf8_strip_invalid(data.check_number);
    if (data.reference_number_valid)
        gnc_utf8_strip_invalid(data.reference_number);

    /***** Create the transaction and setup transaction data *******/
    book = gnc_account_get_book(account);
    transaction = xaccMallocTransaction(book);
    xaccTransBeginEdit(transaction);

    /* Note: Unfortunately libofx <= 0.9.5 will not report a missing
     * date field as an invalid one. Instead, it will report it as
     * valid and return a completely bogus date. Starting with
     * libofx-0.9.6 (not yet released as of 2012-09-09), it will still
     * be reported as valid but at least the date integer itself is
     * just plain zero. */
    if (data.date_posted_valid && (data.date_posted != 0))
    {
        /* The hopeful case: We have a posted_date */
        data.date_posted = fix_ofx_bug_39 (data.date_posted);
        xaccTransSetDatePostedSecsNormalized(transaction, data.date_posted);
    }
    else if (data.date_initiated_valid && (data.date_initiated != 0))
    {
        /* No posted date? Maybe we have an initiated_date */
        data.date_initiated = fix_ofx_bug_39 (data.date_initiated);
        xaccTransSetDatePostedSecsNormalized(transaction, data.date_initiated);
    }
    else
    {
        /* Uh no, no valid date. As a workaround use today's date */
        xaccTransSetDatePostedSecsNormalized(transaction, current_time);
    }

    xaccTransSetDateEnteredSecs(transaction, current_time);

    /* Put transaction name in Description, or memo if name unavailable */
    if (data.name_valid)
    {
        xaccTransSetDescription(transaction, data.name);
    }
    else if (data.memo_valid)
    {
        xaccTransSetDescription(transaction, data.memo);
    }

    /* Put everything else in the Notes field */
    notes = g_strdup_printf("OFX ext. info: ");

    if (data.transactiontype_valid)
    {
        tmp = notes;
        notes = g_strdup_printf("%s%s%s", tmp, "|Trans type:",
                                gnc_ofx_ttype_to_string(data.transactiontype));
        g_free(tmp);
    }

    if (data.invtransactiontype_valid)
    {
        tmp = notes;
        notes = g_strdup_printf("%s%s%s", tmp, "|Investment Trans type:",
                                gnc_ofx_invttype_to_str(data.invtransactiontype));
        g_free(tmp);
    }
    if (data.memo_valid && data.name_valid) /* Copy only if memo wasn't put in Description */
    {
        tmp = notes;
        notes = g_strdup_printf("%s%s%s", tmp, "|Memo:", data.memo);
        g_free(tmp);
    }
    if (data.date_funds_available_valid)
    {
        time64 time = data.date_funds_available;
        gnc_time64_to_iso8601_buff (time, dest_string);
        tmp = notes;
        notes = g_strdup_printf("%s%s%s", tmp,
				"|Date funds available:", dest_string);
        g_free(tmp);
    }
    if (data.server_transaction_id_valid)
    {
        tmp = notes;
        notes = g_strdup_printf("%s%s%s", tmp,
				"|Server trans ID (conf. number):",
				sanitize_string (data.server_transaction_id));
        g_free(tmp);
    }
    if (data.standard_industrial_code_valid)
    {
        tmp = notes;
        notes = g_strdup_printf("%s%s%ld", tmp,
				"|Standard Industrial Code:",
                                data.standard_industrial_code);
        g_free(tmp);

    }
    if (data.payee_id_valid)
    {
        tmp = notes;
        notes = g_strdup_printf("%s%s%s", tmp, "|Payee ID:",
				sanitize_string (data.payee_id));
        g_free(tmp);
    }

    //PERR("WRITEME: GnuCash ofx_proc_transaction():Add PAYEE and ADDRESS here once supported by libofx! Notes=%s\n", notes);

    /* Ideally, gnucash should process the corrected transactions */
    if (data.fi_id_corrected_valid)
    {
        PERR("WRITEME: GnuCash ofx_proc_transaction(): WARNING: This transaction corrected a previous transaction, but we created a new one instead!\n");
        tmp = notes;
        notes = g_strdup_printf("%s%s%s%s", tmp,
				"|This corrects transaction #",
				sanitize_string (data.fi_id_corrected),
				"but GnuCash didn't process the correction!");
        g_free(tmp);
    }
    xaccTransSetNotes(transaction, notes);
    g_free(notes);

    if (data.account_ptr && data.account_ptr->currency_valid)
    {
        DEBUG("Currency from libofx: %s", data.account_ptr->currency);
        currency = gnc_commodity_table_lookup( gnc_get_current_commodities (),
                                               GNC_COMMODITY_NS_CURRENCY,
                                               data.account_ptr->currency);
    }
    else
    {
        DEBUG("Currency from libofx unavailable, defaulting to account's default");
        currency = xaccAccountGetCommodity(account);
    }

    xaccTransSetCurrency(transaction, currency);
    if (data.amount_valid)
    {
        if (!data.invtransactiontype_valid
#ifdef HAVE_LIBOFX_VERSION_0_10
            || data.invtransactiontype == OFX_INVBANKTRAN)
#else
             )
#endif
        {
            double amount = data.amount;
#ifdef HAVE_LIBOFX_VERSION_0_10
            if (data.currency_ratio_valid && data.currency_ratio != 0)
                amount *= data.currency_ratio;
#endif
            /***** Process a normal transaction ******/
            DEBUG("Adding split; Ordinary banking transaction, money flows from or into the source account");
            split = xaccMallocSplit(book);
            xaccTransAppendSplit(transaction, split);
            xaccAccountInsertSplit(account, split);
            gnc_amount = gnc_ofx_numeric_from_double_txn(amount, transaction);
            xaccSplitSetBaseValue(split, gnc_amount, xaccTransGetCurrency(transaction));

            /* set tran-num and/or split-action per book option */
            if (data.check_number_valid)
            {
                gnc_set_num_action(transaction, split, data.check_number, NULL);
            }
            else if (data.reference_number_valid)
            {
                gnc_set_num_action(transaction, split, data.reference_number, NULL);
            }
            /* Also put the ofx transaction's memo in the
             * split's memo field */
            if (data.memo_valid)
            {
                xaccSplitSetMemo(split, data.memo);
            }
            if (data.fi_id_valid)
            {
                gnc_import_set_split_online_id(split,
					       sanitize_string (data.fi_id));
            }
        }

        else if (data.unique_id_valid
                 && data.security_data_valid
                 && data.security_data_ptr != NULL
                 && data.security_data_ptr->secname_valid)
        {
            gboolean choosing_account = TRUE;
	    gnc_utf8_strip_invalid (data.unique_id);
            /********* Process an investment transaction **********/
            /* Note that the ACCT_TYPE_STOCK account type
               should be replaced with something derived from
               data.invtranstype*/

            // We have an investment transaction. First select the correct commodity.
            investment_commodity = gnc_import_select_commodity(data.unique_id,
                                   FALSE,
                                   NULL,
                                   NULL);
            if (investment_commodity != NULL)
            {
                // As we now have the commodity, select the account with that commodity.

                /* Translators: This string is a default account name. It MUST
                 * NOT contain the character ':' anywhere in it or in any
                 * translations.  */
                investment_account_text = g_strdup_printf(
                                         _("Stock account for security \"%s\""),
                             sanitize_string (data.security_data_ptr->secname));

                investment_account_onlineid = g_strdup_printf( "%s%s",
							       data.account_id,
							       data.unique_id);
                investment_account = gnc_import_select_account(
                                     gnc_gen_trans_list_widget(info->gnc_ofx_importer_gui),
                                     investment_account_onlineid,
                                     1,
                                     investment_account_text,
                                     investment_commodity,
                                     ACCT_TYPE_STOCK,
                                     info->last_investment_account,
                                     NULL);
                if (investment_account)
                    info->last_investment_account = investment_account;

                // but use it only if that's really the right commodity
                if (investment_account
                        && xaccAccountGetCommodity(investment_account) != investment_commodity)
                    investment_account = NULL;

                // Loop until we either have an account, or the user pressed Cancel
                while (!investment_account && choosing_account)
                {
                    // No account with correct commodity automatically found.

                    // But are we in auto-create mode and already know a parent?
                    if (auto_create_commodity && ofx_parent_account)
                    {
                        // Yes, so use that as parent when auto-creating the new account below.
                        investment_account = ofx_parent_account;
                    }
                    else
                    {
                        // Let the user choose an account
                        investment_account = gnc_import_select_account(
                                                 gnc_gen_trans_list_widget(info->gnc_ofx_importer_gui),
                                                 data.unique_id,
                                                 TRUE,
                                                 investment_account_text,
                                                 investment_commodity,
                                                 ACCT_TYPE_STOCK,
                                                 info->last_investment_account,
                                                 &choosing_account);
                        if (investment_account)
                            info->last_investment_account = investment_account;
                    }
                    // Does the chosen account have the right commodity?
                    if (investment_account && xaccAccountGetCommodity(investment_account) != investment_commodity)
                    {
                        if (auto_create_commodity
                                && xaccAccountTypesCompatible(xaccAccountGetType(investment_account),
                                                              ACCT_TYPE_STOCK))
                        {
                            // The user chose an account, but it does
                            // not have the right commodity. Also,
                            // auto-creation is on. Hence, we create a
                            // new child account of the selected one,
                            // and this one will have the right
                            // commodity.
                            Account *parent_account = investment_account;
                            investment_account =
                                gnc_ofx_new_account(parent,
                                                    investment_account_text,
                                                    investment_commodity,
                                                    parent_account,
                                                    ACCT_TYPE_STOCK);
                            if (investment_account)
                            {
                                gnc_import_set_acc_online_id(investment_account, data.unique_id);
                                choosing_account = FALSE;
                                ofx_parent_account = parent_account;
                            }
                            else
                            {
                                ofx_parent_account = NULL;
                            }
                        }
                        else
                        {
                            // No account with matching commodity. Ask the user
                            // whether to continue or abort.
                            choosing_account =
                                gnc_verify_dialog(
                                    GTK_WINDOW (gnc_gen_trans_list_widget(info->gnc_ofx_importer_gui)), TRUE,
                                    "The chosen account \"%s\" does not have the correct "
                                    "currency/security \"%s\" (it has \"%s\" instead). "
                                    "This account cannot be used. "
                                    "Do you want to choose again?",
                                    xaccAccountGetName(investment_account),
                                    gnc_commodity_get_fullname(investment_commodity),
                                    gnc_commodity_get_fullname(xaccAccountGetCommodity(investment_account)));
                            // We must also delete the online_id that was set in gnc_import_select_account()
                            gnc_import_set_acc_online_id(investment_account, "");
                            investment_account = NULL;
                        }
                    }
                }
                if (!investment_account)
                {
                    PERR("No investment account found for text: %s\n", investment_account_text);
                }
                g_free (investment_account_text);
                g_free (investment_account_onlineid);
                investment_account_text = NULL;

                if (investment_account != NULL &&
                        data.unitprice_valid &&
                        data.units_valid &&
                        ( data.invtransactiontype != OFX_INCOME ) )
                {
                    DEBUG("Adding investment split; Money flows from or into the stock account");
                    split = xaccMallocSplit(book);
                    xaccTransAppendSplit(transaction, split);
                    xaccAccountInsertSplit(investment_account, split);

                    gnc_amount = gnc_ofx_numeric_from_double_txn (ofx_get_investment_amount(&data),
                                 transaction);
                    gnc_units = gnc_ofx_numeric_from_double (data.units, investment_commodity);
                    xaccSplitSetAmount(split, gnc_units);
                    xaccSplitSetValue(split, gnc_amount);

                    /* set tran-num and/or split-action per book option */
                    if (data.check_number_valid)
                    {
                        gnc_set_num_action(transaction, split, data.check_number, NULL);
                    }
                    else if (data.reference_number_valid)
                    {
                        gnc_set_num_action(transaction, split,
                                           data.reference_number, NULL);
                    }
                    if (data.security_data_ptr->memo_valid)
                    {
                        xaccSplitSetMemo(split,
                                sanitize_string (data.security_data_ptr->memo));
                    }
                    if (data.fi_id_valid)
                    {
                        gnc_import_set_split_online_id(split,
                                                 sanitize_string (data.fi_id));
                    }
                }
                else
                {
                    if (investment_account)
                        PERR("The investment account, units or unitprice was not found for the investment transaction");
                }
            }
            else
            {
                PERR("Commodity not found for the investment transaction");
            }

            if (data.invtransactiontype_valid && investment_account)
            {
                double amount = data.amount;
#ifdef HAVE_LIBOFX_VERSION_0_10
                if (data.currency_ratio_valid && data.currency_ratio != 0)
                    amount *= data.currency_ratio;
#endif
                if (data.invtransactiontype == OFX_REINVEST
                        || data.invtransactiontype == OFX_INCOME)
                {
                    DEBUG("Now let's find an account for the destination split");
                    income_account =
                        get_associated_income_account(investment_account);

                    if (income_account == NULL)
                    {
                        DEBUG("Couldn't find an associated income account");
                        /* Translators: This string is a default account
                         * name. It MUST NOT contain the character ':' anywhere
                         * in it or in any translations.  */
                        investment_account_text = g_strdup_printf(
                                                      _("Income account for security \"%s\""),
                                                      sanitize_string (data.security_data_ptr->secname));
                        income_account = gnc_import_select_account(
                                             gnc_gen_trans_list_widget(info->gnc_ofx_importer_gui),
                                             NULL,
                                             1,
                                             investment_account_text,
                                             currency,
                                             ACCT_TYPE_INCOME,
                                             info->last_income_account,
                                             NULL);
                        if (income_account != NULL)
                        {
                            info->last_income_account = income_account;
                            set_associated_income_account(investment_account,
                                                          income_account);
                            DEBUG("KVP written");
                        }

                    }
                    else
                    {
                        DEBUG("Found at least one associated income account");
                    }
                }
                if (income_account != NULL &&
                        data.invtransactiontype == OFX_REINVEST)
                {
                    DEBUG("Adding investment split; Money flows from the income account");
                    split = xaccMallocSplit(book);
                    xaccTransAppendSplit(transaction, split);
                    xaccAccountInsertSplit(income_account, split);
                    gnc_amount = gnc_ofx_numeric_from_double_txn(amount,
                                                                 transaction);
                    xaccSplitSetBaseValue(split, gnc_amount, xaccTransGetCurrency(transaction));

                    // Set split memo from ofx transaction name or memo
                    gnc_ofx_set_split_memo(&data, split);
                }
                if (income_account != NULL &&
                        data.invtransactiontype == OFX_INCOME)
                {
                    DEBUG("Adding investment split; Money flows from the income account");
                    split = xaccMallocSplit(book);
                    xaccTransAppendSplit(transaction, split);
                    xaccAccountInsertSplit(income_account, split);
                    /*OFX_INCOME amounts come in as positive numbers*/
                    gnc_amount = gnc_ofx_numeric_from_double_txn (-amount,
                                                                  transaction);

                    xaccSplitSetBaseValue(split, gnc_amount, xaccTransGetCurrency(transaction));

                    // Set split memo from ofx transaction name or memo
                    gnc_ofx_set_split_memo(&data, split);
                }
            }

            if (data.invtransactiontype_valid
                    && data.invtransactiontype != OFX_REINVEST)
            {
                DEBUG("Adding investment split; Money flows from or to the cash account");
                split = xaccMallocSplit(book);
                xaccTransAppendSplit(transaction, split);
                xaccAccountInsertSplit(account, split);

                gnc_amount = gnc_ofx_numeric_from_double_txn(
                                 -ofx_get_investment_amount(&data), transaction);
                xaccSplitSetBaseValue(split, gnc_amount,
                                      xaccTransGetCurrency(transaction));

                // Set split memo from ofx transaction name or memo
                gnc_ofx_set_split_memo(&data, split);
            }
        }

        /* Send transaction to importer GUI. */
        if (xaccTransCountSplits(transaction) > 0)
        {
            DEBUG("%d splits sent to the importer gui",
                  xaccTransCountSplits(transaction));
            info->trans_list = g_list_prepend (info->trans_list, transaction);
        }
        else
        {
            PERR("No splits in transaction (missing account?), ignoring.");
            xaccTransDestroy(transaction);
            xaccTransCommitEdit(transaction);
        }
    }
    else
    {
        PERR("The transaction doesn't have a valid amount");
        xaccTransDestroy(transaction);
        xaccTransCommitEdit(transaction);
    }
    info->num_trans_processed += 1;
    return 0;
}//end ofx_proc_transaction()


int ofx_proc_statement_cb (struct OfxStatementData data, void * statement_user_data)
{
    ofx_info* info = (ofx_info*) statement_user_data;
    info->statement = g_new (struct OfxStatementData, 1);
    *info->statement = data;
    return 0;
}


int ofx_proc_account_cb(struct OfxAccountData data, void * account_user_data)
{
    gnc_commodity_table * commodity_table;
    gnc_commodity * default_commodity;
    GNCAccountType default_type = ACCT_TYPE_NONE;
    gchar * account_description;
    GtkWidget * main_widget;
    GtkWidget * parent;
    /* In order to trigger a book options display on the creation of a new book,
     * we need to detect when we are dealing with a new book. */
    gboolean new_book = gnc_is_new_book();
    ofx_info* info = (ofx_info*) account_user_data;
    Account* account = NULL;

    const gchar * account_type_name = _("Unknown OFX account");

    if (data.account_id_valid)
    {
        commodity_table = gnc_get_current_commodities ();
        if (data.currency_valid)
        {
            DEBUG("Currency from libofx: %s", data.currency);
            default_commodity = gnc_commodity_table_lookup(commodity_table,
                                GNC_COMMODITY_NS_CURRENCY,
                                data.currency);
        }
        else
        {
            default_commodity = NULL;
        }

        if (data.account_type_valid)
        {
            switch (data.account_type)
            {
            case OFX_CHECKING :
                default_type = ACCT_TYPE_BANK;
                account_type_name = _("Unknown OFX checking account");
                break;
            case OFX_SAVINGS :
                default_type = ACCT_TYPE_BANK;
                account_type_name = _("Unknown OFX savings account");
                break;
            case OFX_MONEYMRKT :
                default_type = ACCT_TYPE_MONEYMRKT;
                account_type_name = _("Unknown OFX money market account");
                break;
            case OFX_CREDITLINE :
                default_type = ACCT_TYPE_CREDITLINE;
                account_type_name = _("Unknown OFX credit line account");
                break;
            case OFX_CMA :
                default_type = ACCT_TYPE_NONE;
                /* Cash Management Account */
                account_type_name = _("Unknown OFX CMA account");
                break;
            case OFX_CREDITCARD :
                default_type = ACCT_TYPE_CREDIT;
                account_type_name = _("Unknown OFX credit card account");
                break;
            case OFX_INVESTMENT :
                default_type = ACCT_TYPE_BANK;
                account_type_name = _("Unknown OFX investment account");
                break;
            default:
                PERR("WRITEME: ofx_proc_account() This is an unknown account type!");
                break;
            }
        }

        /* If the OFX importer was started in Gnucash in a 'new_book' situation,
         * as described above, the first time the 'ofx_proc_account_cb' function
         * is called a book is created. (This happens after the 'new_book' flag
         * is set in 'gnc_get_current_commodities', called above.) So, before
         * calling 'gnc_import_select_account', allow the user to set book
         * options. */
        if (new_book)
            gnc_new_book_option_display (GTK_WIDGET (gnc_ui_get_main_window (NULL)));

        gnc_utf8_strip_invalid(data.account_name);
        gnc_utf8_strip_invalid(data.account_id);
        account_description = g_strdup_printf (/* This string is a default account
                                                  name. It MUST NOT contain the
                                                  character ':' anywhere in it or
                                                  in any translation.  */
                                               "%s \"%s\"",
                                               account_type_name,
                                               data.account_name);

        main_widget = gnc_gen_trans_list_widget (info->gnc_ofx_importer_gui);

        /* On first use, the import-main-matcher is hidden / not realized so to
         * get a parent use the transient parent of the matcher */
        if (gtk_widget_get_realized (main_widget))
            parent = main_widget;
        else
            parent = GTK_WIDGET(gtk_window_get_transient_for (GTK_WINDOW(main_widget)));

        account = gnc_import_select_account (parent,
                                             data.account_id, 1,
                                             account_description, default_commodity,
                                             default_type, NULL, NULL);

        if (account)
        {
            info->last_import_account = account;
        }

        g_free(account_description);
    }
    else
    {
        PERR("account online ID not available");
    }

    return 0;
}

double ofx_get_investment_amount(const struct OfxTransactionData* data)
{
    double amount = data->amount;
#ifdef HAVE_LIBOFX_VERSION_0_10
    if (data->invtransactiontype == OFX_INVBANKTRAN)
        return 0.0;
    if (data->currency_ratio_valid && data->currency_ratio != 0)
        amount *= data->currency_ratio;
#endif
    g_assert(data);
    switch (data->invtransactiontype)
    {
    case OFX_BUYDEBT:
    case OFX_BUYMF:
    case OFX_BUYOPT:
    case OFX_BUYOTHER:
    case OFX_BUYSTOCK:
        return fabs(amount);
    case OFX_SELLDEBT:
    case OFX_SELLMF:
    case OFX_SELLOPT:
    case OFX_SELLOTHER:
    case OFX_SELLSTOCK:
        return -1 * fabs(amount);
    default:
        return -1 * amount;
    }
}

// Forward declaration, required because several static functions depend on one-another.
static void
gnc_file_ofx_import_process_file (ofx_info* info);

// gnc_ofx_process_next_file processes the next file in the info->file_list.
static void
gnc_ofx_process_next_file (GtkDialog *dialog, gpointer user_data)
{
    ofx_info* info = (ofx_info*) user_data;
    // Free the statement (if it was allocated)
    g_free (info->statement);
    info->statement = NULL;

    // Done with the previous OFX file, process the next one if any.
    info->file_list = g_slist_delete_link (info->file_list, info->file_list);
    if (info->file_list)
        gnc_file_ofx_import_process_file (info);
    else
    {
        // Final cleanup.
        g_free (info);
    }
}

static void
gnc_ofx_on_match_click (GtkDialog *dialog, gint response_id, gpointer user_data)
{
    // Record the response of the user. If cancel we won't go to the next file, etc.
    ofx_info* info = (ofx_info*)user_data;
    info->response = response_id;
}

static void
gnc_ofx_match_done (GtkDialog *dialog, gpointer user_data)
{
    ofx_info* info = (ofx_info*) user_data;

    /* The the user did not click OK, don't process the rest of the
     * transaction, don't go to the next of xfile.
     */
    if (info->response != GTK_RESPONSE_OK) return;

    if (info->trans_list)
    {
         /* Re-run the match dialog if there are transactions
          * remaining in our list (happens if several accounts exist
          * in the same ofx).
          */
        info->gnc_ofx_importer_gui = gnc_gen_trans_list_new (GTK_WIDGET (info->parent), NULL, FALSE, 42, FALSE);
        runMatcher (info, NULL, true);
        return;
    }

    if (info->run_reconcile && info->statement)
    {
        // Open a reconcile window.
        Account* account = gnc_import_select_account (gnc_gen_trans_list_widget(info->gnc_ofx_importer_gui),
                                                      info->statement->account_id,
                                                      0, NULL, NULL, ACCT_TYPE_NONE, NULL, NULL);
        if (account && info->statement->ledger_balance_valid)
        {
            gnc_numeric value = double_to_gnc_numeric (info->statement->ledger_balance,
                                                       xaccAccountGetCommoditySCU (account),
                                                       GNC_HOW_RND_ROUND_HALF_UP);

            RecnWindow* rec_window = recnWindowWithBalance (GTK_WIDGET (info->parent), account, value,
                                                            info->statement->ledger_balance_date);

            // Connect to destroy, at which point we'll process the next OFX file..
            g_signal_connect (G_OBJECT (gnc_ui_reconcile_window_get_window (rec_window)), "destroy",
                              G_CALLBACK (gnc_ofx_process_next_file), info);
            return;
        }
    }
    gnc_ofx_process_next_file (NULL, info);
}

// This callback is triggered when the user checks or unchecks the reconcile after match
// check box in the matching dialog.
static void
reconcile_when_close_toggled_cb (GtkToggleButton *togglebutton, ofx_info* info)
{
    info->run_reconcile = gtk_toggle_button_get_active (togglebutton);
}

static void
runMatcher(ofx_info* info, char * selected_filename, gboolean go_to_next_file)
{
    GtkWindow *parent = info->parent;
    GList* trans_list_remain = NULL;
    Account* first_account = NULL;

    /* If we have multiple accounts in the ofx file, we need to
     * process transactions one account at a time, in case there are
     * transfers between accounts.
     */
    info->num_trans_processed = 0;
    for(GList* node = info->trans_list; node; node=node->next)
    {
        Transaction* trans = node->data;
        Split* split = xaccTransGetSplit (trans, 0);
        if (first_account == NULL) first_account = xaccSplitGetAccount (split);
        if (xaccSplitGetAccount (split) == first_account)
        {
            gnc_gen_trans_list_add_trans (info->gnc_ofx_importer_gui, trans);
            info->num_trans_processed ++;
        }
        else trans_list_remain = g_list_prepend (trans_list_remain, trans);
    }
    g_list_free (info->trans_list);
    info->trans_list = g_list_reverse (trans_list_remain);

    // See whether the view has anything in it and warn the user if not.
    if (gnc_gen_trans_list_empty (info->gnc_ofx_importer_gui))
    {
        gnc_gen_trans_list_delete (info->gnc_ofx_importer_gui);
        if (info->num_trans_processed)
        {
            gchar* acct_name = gnc_get_account_name_for_register (first_account);
            gnc_info_dialog (parent, _("OFX file '%s', imported transactions for account '%s'\n%d transactions processed, no transactions to match"),
                             selected_filename, acct_name, info->num_trans_processed);
            g_free (acct_name);
            gnc_ofx_match_done (NULL,info);
            return;
        }
    }
    else
    {
        /* Show the match dialog and connect to the "destroy" signal so we can trigger a reconcile when
         the user clicks OK when done matching transactions if required. Connecting to response isn't enough
         because only when the matcher is destroyed do imported transactions get recorded */
        g_signal_connect (G_OBJECT (gnc_gen_trans_list_widget (info->gnc_ofx_importer_gui)), "destroy",
                          G_CALLBACK (gnc_ofx_match_done), info);
        
        // Connect to response so we know if the user pressed "cancel".
        g_signal_connect (G_OBJECT (gnc_gen_trans_list_widget (info->gnc_ofx_importer_gui)), "response",
                          G_CALLBACK (gnc_ofx_on_match_click), info);
        
        gnc_gen_trans_list_show_all (info->gnc_ofx_importer_gui);
        
        // Show or hide the check box for reconciling after match, depending on whether a statement was received.
        gnc_gen_trans_list_show_reconcile_after_close_button (info->gnc_ofx_importer_gui, info->statement != NULL, info->run_reconcile);
        
        // Finally connect to the reconcile after match check box so we can be notified if the user wants/does not want to reconcile.
        g_signal_connect (G_OBJECT (gnc_gen_trans_list_get_reconcile_after_close_button (info->gnc_ofx_importer_gui)), "toggled",
                          G_CALLBACK (reconcile_when_close_toggled_cb), info);
    }
}

// Aux function to process the OFX file in info->file_list
static void
gnc_file_ofx_import_process_file (ofx_info* info)
{
    LibofxContextPtr libofx_context;
    char* filename = NULL;
    char * selected_filename = NULL;
    GtkWindow *parent = info->parent;

    if (info->file_list == NULL)
        return;

    filename = info->file_list->data;
    libofx_context = libofx_get_new_context();

#ifdef G_OS_WIN32
    selected_filename = g_win32_locale_filename_from_utf8 (filename);
    g_free (filename);
#else
    selected_filename = filename;
#endif
    DEBUG("Filename found: %s", selected_filename);

    // Reset the reconciliation information.
    info->num_trans_processed = 0;
    info->statement = NULL;

    /* Initialize libofx and set the callbacks*/
    ofx_set_statement_cb (libofx_context, ofx_proc_statement_cb, info);
    ofx_set_account_cb (libofx_context, ofx_proc_account_cb, info);
    ofx_set_transaction_cb (libofx_context, ofx_proc_transaction_cb, info);
    ofx_set_security_cb (libofx_context, ofx_proc_security_cb, info);
    /*ofx_set_status_cb(libofx_context, ofx_proc_status_cb, 0);*/

    // Create the match dialog, and run the ofx file through the importer.
    info->gnc_ofx_importer_gui = gnc_gen_trans_list_new (GTK_WIDGET(parent), NULL, FALSE, 42, FALSE);
    libofx_proc_file (libofx_context, selected_filename, AUTODETECT);

    // Free the libofx context before recursing to process the next file
    libofx_free_context(libofx_context);
    runMatcher(info, selected_filename,true);
    g_free(selected_filename);
}

// The main import function. Starts the chain of file imports (if there are several)
void gnc_file_ofx_import (GtkWindow *parent)
{
    extern int ofx_PARSER_msg;
    extern int ofx_DEBUG_msg;
    extern int ofx_WARNING_msg;
    extern int ofx_ERROR_msg;
    extern int ofx_INFO_msg;
    extern int ofx_STATUS_msg;
    GSList* selected_filenames = NULL;
    char *default_dir;
    GList *filters = NULL;
    GSList* iter = NULL;
    ofx_info* info = NULL;
    GtkFileFilter* filter = gtk_file_filter_new ();


    ofx_PARSER_msg = false;
    ofx_DEBUG_msg = false;
    ofx_WARNING_msg = true;
    ofx_ERROR_msg = true;
    ofx_INFO_msg = true;
    ofx_STATUS_msg = false;

    DEBUG("gnc_file_ofx_import(): Begin...\n");

    default_dir = gnc_get_default_directory(GNC_PREFS_GROUP);
    gtk_file_filter_set_name (filter, _("Open/Quicken Financial Exchange file (*.ofx, *.qfx)"));
    gtk_file_filter_add_pattern (filter, "*.[oqOQ][fF][xX]");
    filters = g_list_prepend( filters, filter );

    selected_filenames = gnc_file_dialog_multi (parent,
                                                _("Select one or multiple OFX/QFX file(s) to process"),
                                                filters,
                                                default_dir,
                                                GNC_FILE_DIALOG_IMPORT);
    g_free(default_dir);

    if (selected_filenames)
    {
        /* Remember the directory as the default. */
        default_dir = g_path_get_dirname(selected_filenames->data);
        gnc_set_default_directory(GNC_PREFS_GROUP, default_dir);
        g_free(default_dir);

        /* Look up the needed preferences */
        auto_create_commodity =
            gnc_prefs_get_bool (GNC_PREFS_GROUP_IMPORT, GNC_PREF_AUTO_COMMODITY);

        DEBUG("Opening selected file(s)");
        // Create the structure that holds the list of files to process and the statement info.
        info = g_new(ofx_info,1);
        info->num_trans_processed = 0;
        info->statement = NULL;
        info->last_investment_account = NULL;
        info->last_import_account = NULL;
        info->last_income_account = NULL;
        info->parent = parent;
        info->run_reconcile = FALSE;
        info->file_list = selected_filenames;
        info->trans_list = NULL;
        info->response = 0;
        // Call the aux import function.
        gnc_file_ofx_import_process_file (info);
    }
}


/** @} */
