/********************************************************************\
 * Scrub.c -- convert single-entry accounts into clean double-entry *
 *                                                                  *
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
 *                                                                  *
\********************************************************************/

/*
 * FILE:
 * Scrub.c
 *
 * FUNCTION:
 * Provides a set of functions and utilities for scrubbing clean
 * single-entry accounts so that they can be promoted into
 * self-consistent, clean double-entry accounts.
 *
 * HISTORY:
 * Created by Linas Vepstas December 1998
 * Copyright (c) 1998-2000, 2003 Linas Vepstas <linas@linas.org>
 * Copyright (c) 2002 Christian Stimming
 * Copyright (c) 2006 David Hampton
 */

#include <config.h>

#include <glib.h>
#include <glib/gi18n.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "Account.h"
#include "AccountP.h"
#include "Scrub.h"
#include "Transaction.h"
#include "TransactionP.h"
#include "gnc-commodity.h"
#include "qofinstance-p.h"
#include "gnc-session.h"

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gnc.engine.scrub"

static QofLogModule log_module = G_LOG_DOMAIN;
static gboolean abort_now = FALSE;
static gint scrub_depth = 0;


static Account* xaccScrubUtilityGetOrMakeAccount (Account *root,
                                                  gnc_commodity* currency,
                                                  const char* accname,
                                                  GNCAccountType acctype,
                                                  gboolean placeholder,
                                                  gboolean checkname);

void
gnc_set_abort_scrub (gboolean abort)
{
    abort_now = abort;
}

gboolean
gnc_get_abort_scrub (void)
{
    return abort_now;
}

gboolean
gnc_get_ongoing_scrub (void)
{
    return scrub_depth > 0;
}

/* ================================================================ */

void
xaccAccountTreeScrubOrphans (Account *acc, QofPercentageFunc percentagefunc)
{
    if (!acc) return;

    if (abort_now)
        (percentagefunc)(NULL, -1.0);

    scrub_depth ++;
    xaccAccountScrubOrphans (acc, percentagefunc);
    gnc_account_foreach_descendant(acc,
                                   (AccountCb)xaccAccountScrubOrphans, percentagefunc);
    scrub_depth--;
}

static void
TransScrubOrphansFast (Transaction *trans, Account *root)
{
    GList *node;
    gchar *accname;

    if (!trans) return;
    g_return_if_fail (root);
    g_return_if_fail (trans->common_currency);

    for (node = trans->splits; node; node = node->next)
    {
        Split *split = node->data;
        Account *orph;
        if (abort_now) break;

        if (split->acc) continue;

        DEBUG ("Found an orphan\n");

        accname = g_strconcat (_("Orphan"), "-",
                               gnc_commodity_get_mnemonic (trans->common_currency),
                               NULL);
        orph = xaccScrubUtilityGetOrMakeAccount (root, trans->common_currency,
                                                 accname, ACCT_TYPE_BANK,
                                                 FALSE, TRUE);
        g_free (accname);
        if (!orph) continue;

        xaccSplitSetAccount(split, orph);
    }
}

void
xaccAccountScrubOrphans (Account *acc, QofPercentageFunc percentagefunc)
{
    GList *node, *splits;
    const char *str;
    const char *message = _( "Looking for orphans in account %s: %u of %u");
    guint total_splits = 0;
    guint current_split = 0;

    if (!acc) return;
    scrub_depth++;

    str = xaccAccountGetName (acc);
    str = str ? str : "(null)";
    PINFO ("Looking for orphans in account %s\n", str);
    splits = xaccAccountGetSplitList(acc);
    total_splits = g_list_length (splits);

    for (node = splits; node; node = node->next)
    {
        Split *split = node->data;
        if (current_split % 10 == 0)
        {
            char *progress_msg = g_strdup_printf (message, str, current_split, total_splits);
            (percentagefunc)(progress_msg, (100 * current_split) / total_splits);
            g_free (progress_msg);
            if (abort_now) break;
        }

        TransScrubOrphansFast (xaccSplitGetParent (split),
                               gnc_account_get_root (acc));
        current_split++;
    }
    (percentagefunc)(NULL, -1.0);
    scrub_depth--;
}


void
xaccTransScrubOrphans (Transaction *trans)
{
    SplitList *node;
    QofBook *book = NULL;
    Account *root = NULL;

    if (!trans) return;

    for (node = trans->splits; node; node = node->next)
    {
        Split *split = node->data;
        if (abort_now) break;

        if (split->acc)
        {
            TransScrubOrphansFast (trans, gnc_account_get_root(split->acc));
            return;
        }
    }

    /* If we got to here, then *none* of the splits belonged to an
     * account.  Not a happy situation.  We should dig an account
     * out of the book the transaction belongs to.
     * XXX we should probably *always* to this, instead of the above loop!
     */
    PINFO ("Free Floating Transaction!");
    book = xaccTransGetBook (trans);
    root = gnc_book_get_root_account (book);
    TransScrubOrphansFast (trans, root);
}

/* ================================================================ */

void
xaccAccountTreeScrubSplits (Account *account)
{
    if (!account) return;

    xaccAccountScrubSplits (account);
    gnc_account_foreach_descendant(account,
                                   (AccountCb)xaccAccountScrubSplits, NULL);
}

void
xaccAccountScrubSplits (Account *account)
{
    GList *node;
    scrub_depth++;
    for (node = xaccAccountGetSplitList (account); node; node = node->next)
    {
        if (abort_now) break;
        xaccSplitScrub (node->data);
    }
    scrub_depth--;
}

void
xaccSplitScrub (Split *split)
{
    Account *account;
    Transaction *trans;
    gnc_numeric value, amount;
    gnc_commodity *currency, *acc_commodity;
    int scu;

    if (!split) return;
    ENTER ("(split=%p)", split);

    trans = xaccSplitGetParent (split);
    if (!trans)
    {
        LEAVE("no trans");
        return;
    }

    account = xaccSplitGetAccount (split);

    /* If there's no account, this split is an orphan.
     * We need to fix that first, before proceeding.
     */
    if (!account)
    {
        xaccTransScrubOrphans (trans);
        account = xaccSplitGetAccount (split);
    }

    /* Grrr... the register gnc_split_register_load() line 203 of
     *  src/register/ledger-core/split-register-load.c will create
     * free-floating bogus transactions. Ignore these for now ...
     */
    if (!account)
    {
        PINFO ("Free Floating Transaction!");
        LEAVE ("no account");
        return;
    }

    /* Split amounts and values should be valid numbers */
    value = xaccSplitGetValue (split);
    if (gnc_numeric_check (value))
    {
        value = gnc_numeric_zero();
        xaccSplitSetValue (split, value);
    }

    amount = xaccSplitGetAmount (split);
    if (gnc_numeric_check (amount))
    {
        amount = gnc_numeric_zero();
        xaccSplitSetAmount (split, amount);
    }

    currency = xaccTransGetCurrency (trans);

    /* If the account doesn't have a commodity,
     * we should attempt to fix that first.
     */
    acc_commodity = xaccAccountGetCommodity(account);
    if (!acc_commodity)
    {
        xaccAccountScrubCommodity (account);
    }
    if (!acc_commodity || !gnc_commodity_equiv(acc_commodity, currency))
    {
        LEAVE ("(split=%p) inequiv currency", split);
        return;
    }

    scu = MIN (xaccAccountGetCommoditySCU (account),
               gnc_commodity_get_fraction (currency));

    if (gnc_numeric_same (amount, value, scu, GNC_HOW_RND_ROUND_HALF_UP))
    {
        LEAVE("(split=%p) different values", split);
        return;
    }

    /*
     * This will be hit every time you answer yes to the dialog "The
     * current transaction has changed. Would you like to record it.
     */
    PINFO ("Adjusted split with mismatched values, desc=\"%s\" memo=\"%s\""
           " old amount %s %s, new amount %s",
           trans->description, split->memo,
           gnc_num_dbg_to_string (xaccSplitGetAmount(split)),
           gnc_commodity_get_mnemonic (currency),
           gnc_num_dbg_to_string (xaccSplitGetValue(split)));

    xaccTransBeginEdit (trans);
    xaccSplitSetAmount (split, value);
    xaccTransCommitEdit (trans);
    LEAVE ("(split=%p)", split);
}

/* ================================================================ */

void
xaccAccountTreeScrubImbalance (Account *acc, QofPercentageFunc percentagefunc)
{
    if (!acc) return;

    if (abort_now)
        (percentagefunc)(NULL, -1.0);

    scrub_depth++;
    xaccAccountScrubImbalance (acc, percentagefunc);
    gnc_account_foreach_descendant(acc,
                                   (AccountCb)xaccAccountScrubImbalance, percentagefunc);
    scrub_depth--;
}

void
xaccAccountScrubImbalance (Account *acc, QofPercentageFunc percentagefunc)
{
    GList *node, *splits;
    const char *str;
    const char *message = _( "Looking for imbalances in account %s: %u of %u");
    gint split_count = 0, curr_split_no = 0;

    if (!acc) return;
    scrub_depth++;

    str = xaccAccountGetName(acc);
    str = str ? str : "(null)";
    PINFO ("Looking for imbalances in account %s\n", str);

    splits = xaccAccountGetSplitList(acc);
    split_count = g_list_length (splits);
    for (node = splits; node; node = node->next)
    {
        Split *split = node->data;
        Transaction *trans = xaccSplitGetParent(split);
        if (abort_now) break;

        PINFO("Start processing split %d of %d",
              curr_split_no + 1, split_count);

        if (curr_split_no % 10 == 0)
        {
            char *progress_msg = g_strdup_printf (message, str, curr_split_no, split_count);
            (percentagefunc)(progress_msg, (100 * curr_split_no) / split_count);
            g_free (progress_msg);
        }

        TransScrubOrphansFast (xaccSplitGetParent (split),
                               gnc_account_get_root (acc));

        xaccTransScrubCurrency(trans);

        xaccTransScrubImbalance (trans, gnc_account_get_root (acc), NULL);

        PINFO("Finished processing split %d of %d",
              curr_split_no + 1, split_count);
        curr_split_no++;
    }
    (percentagefunc)(NULL, -1.0);
    scrub_depth--;
}

static Split *
get_balance_split (Transaction *trans, Account *root, Account *account,
                   gnc_commodity *commodity)
{
    Split *balance_split;
    gchar *accname;

    if (!account ||
        !gnc_commodity_equiv (commodity, xaccAccountGetCommodity(account)))
    {
        if (!root)
        {
            root = gnc_book_get_root_account (xaccTransGetBook (trans));
            if (NULL == root)
            {
                /* This can't occur, things should be in books */
                PERR ("Bad data corruption, no root account in book");
                return NULL;
            }
        }
        accname = g_strconcat (_("Imbalance"), "-",
                               gnc_commodity_get_mnemonic (commodity), NULL);
        account = xaccScrubUtilityGetOrMakeAccount (root, commodity,
                                                    accname, ACCT_TYPE_BANK,
                                                    FALSE, TRUE);
        g_free (accname);
        if (!account)
        {
            PERR ("Can't get balancing account");
            return NULL;
        }
    }

    balance_split = xaccTransFindSplitByAccount(trans, account);

    /* Put split into account before setting split value */
    if (!balance_split)
    {
        balance_split = xaccMallocSplit (qof_instance_get_book(trans));

        xaccTransBeginEdit (trans);
        xaccSplitSetParent(balance_split, trans);
        xaccSplitSetAccount(balance_split, account);
        xaccTransCommitEdit (trans);
    }

    return balance_split;
}

static gnc_commodity*
find_root_currency(void)
{
    QofSession *sess = gnc_get_current_session ();
    Account *root = gnc_book_get_root_account (qof_session_get_book (sess));
    gnc_commodity *root_currency = xaccAccountGetCommodity (root);

    /* Some older books may not have a currency set on the root
     * account. In that case find the first top-level INCOME account
     * and use its currency. */
    if (!root_currency)
    {
         GList *children = gnc_account_get_children (root);
         for (GList *node = children; node && !root_currency;
              node = g_list_next (node))
         {
              Account *child = GNC_ACCOUNT (node->data);
              if (xaccAccountGetType (child) == ACCT_TYPE_INCOME)
                   root_currency = xaccAccountGetCommodity (child);
         }
         g_list_free (children);
    }
    return root_currency;
}

/* Get the trading split for a given commodity, creating it (and the
   necessary parent accounts) if it doesn't exist. */
static Split *
get_trading_split (Transaction *trans, Account *base,
                   gnc_commodity *commodity)
{
    Split *balance_split;
    Account *trading_account;
    Account *ns_account;
    Account *account;
    Account* root = gnc_book_get_root_account (xaccTransGetBook (trans));
    gnc_commodity *root_currency = find_root_currency ();

    trading_account = xaccScrubUtilityGetOrMakeAccount (root,
                                                        NULL,
                                                        _("Trading"),
                                                        ACCT_TYPE_TRADING,
                                                        TRUE, FALSE);
    if (!trading_account)
    {
        PERR ("Can't get trading account");
        return NULL;
    }

    ns_account = xaccScrubUtilityGetOrMakeAccount (trading_account,
                                                   NULL,
                                                   gnc_commodity_get_namespace(commodity),
                                                   ACCT_TYPE_TRADING,
                                                   TRUE, TRUE);
    if (!ns_account)
    {
        PERR ("Can't get namespace account");
        return NULL;
    }

    account = xaccScrubUtilityGetOrMakeAccount (ns_account, commodity,
                                                gnc_commodity_get_mnemonic(commodity),
                                                ACCT_TYPE_TRADING,
                                                FALSE, FALSE);
    if (!account)
    {
        PERR ("Can't get commodity account");
        return NULL;
    }


    balance_split = xaccTransFindSplitByAccount(trans, account);

    /* Put split into account before setting split value */
    if (!balance_split)
    {
        balance_split = xaccMallocSplit (qof_instance_get_book(trans));

        xaccTransBeginEdit (trans);
        xaccSplitSetParent(balance_split, trans);
        xaccSplitSetAccount(balance_split, account);
        xaccTransCommitEdit (trans);
    }

    return balance_split;
}

static void
add_balance_split (Transaction *trans, gnc_numeric imbalance,
                   Account *root, Account *account)
{
    const gnc_commodity *commodity;
    gnc_numeric old_value, new_value;
    Split *balance_split;
    gnc_commodity *currency = xaccTransGetCurrency (trans);

    balance_split = get_balance_split(trans, root, account, currency);
    if (!balance_split)
    {
        /* Error already logged */
        LEAVE("");
        return;
    }
    account = xaccSplitGetAccount(balance_split);

    xaccTransBeginEdit (trans);

    old_value = xaccSplitGetValue (balance_split);

    /* Note: We have to round for the commodity's fraction, NOT any
     * already existing denominator (bug #104343), because either one
     * of the denominators might already be reduced.  */
    new_value = gnc_numeric_sub (old_value, imbalance,
                                 gnc_commodity_get_fraction(currency),
                                 GNC_HOW_RND_ROUND_HALF_UP);

    xaccSplitSetValue (balance_split, new_value);

    commodity = xaccAccountGetCommodity (account);
    if (gnc_commodity_equiv (currency, commodity))
    {
        xaccSplitSetAmount (balance_split, new_value);
    }

    xaccSplitScrub (balance_split);
    xaccTransCommitEdit (trans);
}

/* Balance a transaction without trading accounts. */
static void
gnc_transaction_balance_no_trading (Transaction *trans, Account *root,
                                    Account *account)
{
    gnc_numeric imbalance  = xaccTransGetImbalanceValue (trans);

    /* Make the value sum to zero */
    if (! gnc_numeric_zero_p (imbalance))
    {
        PINFO ("Value unbalanced transaction");

        add_balance_split (trans, imbalance, root, account);
    }

}

static gnc_numeric
gnc_transaction_get_commodity_imbalance (Transaction *trans,
                                         gnc_commodity *commodity)
{
    /* Find the value imbalance in this commodity */
    gnc_numeric val_imbalance = gnc_numeric_zero();
    GList *splits = NULL;
    for (splits = trans->splits; splits; splits = splits->next)
    {
        Split *split = splits->data;
        gnc_commodity *split_commodity =
            xaccAccountGetCommodity(xaccSplitGetAccount(split));
        if (xaccTransStillHasSplit (trans, split) &&
            gnc_commodity_equal (commodity, split_commodity))
            val_imbalance = gnc_numeric_add (val_imbalance,
                                             xaccSplitGetValue (split),
                                             GNC_DENOM_AUTO,
                                             GNC_HOW_DENOM_EXACT);
    }
    return val_imbalance;
}

/* GFunc wrapper for xaccSplitDestroy */
static void
destroy_split (void* ptr)
{
    Split *split = GNC_SPLIT (ptr);
    if (split)
        xaccSplitDestroy (split);
}

/* Balancing transactions with trading accounts works best when
 * starting with no trading splits.
 */
static void
xaccTransClearTradingSplits (Transaction *trans)
{
    GList *trading_splits = NULL;

    for (GList* node = trans->splits; node; node = node->next)
    {
         Split* split = GNC_SPLIT(node->data);
         Account* acc = NULL;
         if (!split)
              continue;
         acc = xaccSplitGetAccount(split);
         if (acc && xaccAccountGetType(acc) == ACCT_TYPE_TRADING)
            trading_splits = g_list_prepend (trading_splits, node->data);
    }

    if (!trading_splits)
        return;

    xaccTransBeginEdit (trans);
    /* destroy_splits doesn't actually free the splits but this gets
     * the list ifself freed.
     */
    g_list_free_full (trading_splits, destroy_split);
    xaccTransCommitEdit (trans);
}

static void
gnc_transaction_balance_trading (Transaction *trans, Account *root)
{
    MonetaryList *imbal_list;
    MonetaryList *imbalance_commod;
    Split *balance_split = NULL;

    /* If the transaction is balanced, nothing more to do */
    imbal_list = xaccTransGetImbalance (trans);
    if (!imbal_list)
    {
        LEAVE("transaction is balanced");
        return;
    }

    PINFO ("Currency unbalanced transaction");

    for (imbalance_commod = imbal_list; imbalance_commod;
         imbalance_commod = imbalance_commod->next)
    {
        gnc_monetary *imbal_mon = imbalance_commod->data;
        gnc_commodity *commodity;
        gnc_numeric old_amount, new_amount;
        const gnc_commodity *txn_curr = xaccTransGetCurrency (trans);

        commodity = gnc_monetary_commodity (*imbal_mon);

        balance_split = get_trading_split(trans, root, commodity);
        if (!balance_split)
        {
            /* Error already logged */
            gnc_monetary_list_free(imbal_list);
            LEAVE("");
            return;
        }

        xaccTransBeginEdit (trans);

        old_amount = xaccSplitGetAmount (balance_split);
        new_amount = gnc_numeric_sub (old_amount, gnc_monetary_value(*imbal_mon),
                                      gnc_commodity_get_fraction(commodity),
                                      GNC_HOW_RND_ROUND_HALF_UP);

        xaccSplitSetAmount (balance_split, new_amount);

        if (gnc_commodity_equal (txn_curr, commodity))
        {
            /* Imbalance commodity is the transaction currency, value in the
               split must be the same as the amount */
            xaccSplitSetValue (balance_split, new_amount);
        }
        else
        {
            gnc_numeric val_imbalance = gnc_transaction_get_commodity_imbalance (trans,            commodity);

            gnc_numeric old_value = xaccSplitGetValue (balance_split);
            gnc_numeric new_value = gnc_numeric_sub (old_value, val_imbalance,
                                         gnc_commodity_get_fraction(txn_curr),
                                         GNC_HOW_RND_ROUND_HALF_UP);

            xaccSplitSetValue (balance_split, new_value);
        }

        xaccSplitScrub (balance_split);
        xaccTransCommitEdit (trans);
    }

    gnc_monetary_list_free(imbal_list);
}

/** Balance the transaction by adding more trading splits. This shouldn't
 * ordinarily be necessary.
 * @param trans the transaction to balance
 * @param root the root account
 */
static void
gnc_transaction_balance_trading_more_splits (Transaction *trans, Account *root)
{
    /* Copy the split list so we don't see the splits we're adding */
    GList *splits_dup = g_list_copy(trans->splits), *splits = NULL;
    const gnc_commodity  *txn_curr = xaccTransGetCurrency (trans);
    for (splits = splits_dup; splits; splits = splits->next)
    {
        Split *split = splits->data;
        if (! xaccTransStillHasSplit(trans, split)) continue;
        if (!gnc_numeric_zero_p(xaccSplitGetValue(split)) &&
            gnc_numeric_zero_p(xaccSplitGetAmount(split)))
        {
            gnc_commodity *commodity;
            gnc_numeric old_value, new_value;
            Split *balance_split;

            commodity = xaccAccountGetCommodity(xaccSplitGetAccount(split));
            if (!commodity)
            {
                PERR("Split has no commodity");
                continue;
            }
            balance_split = get_trading_split(trans, root, commodity);
            if (!balance_split)
            {
                /* Error already logged */
                LEAVE("");
                return;
            }
            xaccTransBeginEdit (trans);

            old_value = xaccSplitGetValue (balance_split);
            new_value = gnc_numeric_sub (old_value, xaccSplitGetValue(split),
                                         gnc_commodity_get_fraction(txn_curr),
                                         GNC_HOW_RND_ROUND_HALF_UP);
            xaccSplitSetValue (balance_split, new_value);

            /* Don't change the balance split's amount since the amount
               is zero in the split we're working on */

            xaccSplitScrub (balance_split);
            xaccTransCommitEdit (trans);
        }
    }

    g_list_free(splits_dup);
}

/** Correct transaction imbalances.
 * @param trans The Transaction
 * @param root The (hidden) root account, for the book default currency.
 * @param account The account whose currency in which to balance.
 */

void
xaccTransScrubImbalance (Transaction *trans, Account *root,
                         Account *account)
{
    gnc_numeric imbalance;

    if (!trans) return;

    ENTER ("()");

    /* Must look for orphan splits and remove trading splits even if
     * there is no imbalance and we're not using trading accounts.
     */
    xaccTransScrubSplits (trans);
    xaccTransClearTradingSplits (trans);

    /* Return immediately if things are balanced. */
    if (xaccTransIsBalanced (trans))
    {
        LEAVE ("transaction is balanced");
        return;
    }

    if (! xaccTransUseTradingAccounts (trans))
    {
        gnc_transaction_balance_no_trading (trans, root, account);
        LEAVE ("transaction balanced, no trading accounts");
        return;
    }

    imbalance = xaccTransGetImbalanceValue (trans);
    if (! gnc_numeric_zero_p (imbalance))
    {
        PINFO ("Value unbalanced transaction");

        add_balance_split (trans, imbalance, root, account);
    }

    gnc_transaction_balance_trading (trans, root);
    if (gnc_numeric_zero_p(xaccTransGetImbalanceValue(trans)))
    {
        LEAVE ("()");
        return;
    }
    /* If the transaction is still not balanced, it's probably because there
       are splits with zero amount and non-zero value.  These are usually
       realized gain/loss splits.  Add a reversing split for each of them to
       balance the value. */

    gnc_transaction_balance_trading_more_splits (trans, root);
    if (!gnc_numeric_zero_p(xaccTransGetImbalanceValue(trans)))
        PERR("Balancing currencies unbalanced value");

}

/* ================================================================ */
/* The xaccTransFindCommonCurrency () method returns
 *    a gnc_commodity indicating a currency denomination that all
 *    of the splits in this transaction have in common, using the
 *    old/obsolete currency/security fields of the split accounts.
 */

static gnc_commodity *
FindCommonExclSCurrency (SplitList *splits,
                         gnc_commodity * ra, gnc_commodity * rb,
                         Split *excl_split)
{
    GList *node;

    if (!splits) return NULL;

    for (node = splits; node; node = node->next)
    {
        Split *s = node->data;
        gnc_commodity * sa, * sb;

        if (s == excl_split) continue;

        g_return_val_if_fail (s->acc, NULL);

        sa = DxaccAccountGetCurrency (s->acc);
        sb = xaccAccountGetCommodity (s->acc);

        if (ra && rb)
        {
            int aa = !gnc_commodity_equiv(ra, sa);
            int ab = !gnc_commodity_equiv(ra, sb);
            int ba = !gnc_commodity_equiv(rb, sa);
            int bb = !gnc_commodity_equiv(rb, sb);

            if ( (!aa) && bb) rb = NULL;
            else if ( (!ab) && ba) rb = NULL;
            else if ( (!ba) && ab) ra = NULL;
            else if ( (!bb) && aa) ra = NULL;
            else if ( aa && bb && ab && ba )
            {
                ra = NULL;
                rb = NULL;
            }

            if (!ra)
            {
                ra = rb;
                rb = NULL;
            }
        }
        else if (ra && !rb)
        {
            int aa = !gnc_commodity_equiv(ra, sa);
            int ab = !gnc_commodity_equiv(ra, sb);
            if ( aa && ab ) ra = NULL;
        }
        else if (!ra && rb)
        {
            int aa = !gnc_commodity_equiv(rb, sa);
            int ab = !gnc_commodity_equiv(rb, sb);
            ra = ( aa && ab ) ? NULL : rb;
        }

        if ((!ra) && (!rb)) return NULL;
    }

    return (ra);
}

/* This is the wrapper for those calls (i.e. the older ones) which
 * don't exclude one split from the splitlist when looking for a
 * common currency.
 */
static gnc_commodity *
FindCommonCurrency (GList *splits, gnc_commodity * ra, gnc_commodity * rb)
{
    return FindCommonExclSCurrency(splits, ra, rb, NULL);
}

static gnc_commodity *
xaccTransFindOldCommonCurrency (Transaction *trans, QofBook *book)
{
    gnc_commodity *ra, *rb, *retval;
    Split *split;

    if (!trans) return NULL;

    if (trans->splits == NULL) return NULL;

    g_return_val_if_fail (book, NULL);

    split = trans->splits->data;

    if (!split || NULL == split->acc) return NULL;

    ra = DxaccAccountGetCurrency (split->acc);
    rb = xaccAccountGetCommodity (split->acc);

    retval = FindCommonCurrency (trans->splits, ra, rb);

    if (retval && !gnc_commodity_is_currency(retval))
        retval = NULL;

    return retval;
}

/* Test the currency of the splits and find the most common and return
 * it, or NULL if there is no currency more common than the
 * others -- or none at all.
 */
typedef struct
{
    gnc_commodity *commodity;
    unsigned int count;
} CommodityCount;

static gint
commodity_equal (gconstpointer a, gconstpointer b)
{
    CommodityCount *cc = (CommodityCount*)a;
    gnc_commodity *com = (gnc_commodity*)b;
    if ( cc == NULL || cc->commodity == NULL ||
         !GNC_IS_COMMODITY( cc->commodity ) ) return -1;
    if ( com == NULL || !GNC_IS_COMMODITY( com ) ) return 1;
    if ( gnc_commodity_equal(cc->commodity, com) )
        return 0;
    return 1;
}

static gint
commodity_compare( gconstpointer a, gconstpointer b)
{
    CommodityCount *ca = (CommodityCount*)a, *cb = (CommodityCount*)b;
    if (ca == NULL || ca->commodity == NULL ||
        !GNC_IS_COMMODITY( ca->commodity ) )
    {
        if (cb == NULL || cb->commodity == NULL ||
            !GNC_IS_COMMODITY( cb->commodity ) )
            return 0;
        return -1;
    }
    if (cb == NULL || cb->commodity == NULL ||
        !GNC_IS_COMMODITY( cb->commodity ) )
        return 1;
    if (ca->count == cb->count)
        return 0;
    return ca->count > cb->count ? 1 : -1;
}

/* Find the commodities in the account of each of the splits of a
 * transaction, and rank them by how many splits in which they
 * occur. Commodities which are currencies count more than those which
 * aren't, because for simple buy and sell transactions it makes
 * slightly more sense for the transaction commodity to be the
 * currency -- to the extent that it makes sense for a transaction to
 * have a currency at all. jralls, 2010-11-02 */

static gnc_commodity *
xaccTransFindCommonCurrency (Transaction *trans, QofBook *book)
{
    gnc_commodity *com_scratch;
    GList *node = NULL;
    GSList *comlist = NULL, *found = NULL;

    if (!trans) return NULL;

    if (trans->splits == NULL) return NULL;

    g_return_val_if_fail (book, NULL);

    /* Find the most commonly used currency among the splits.  If a given split
       is in a non-currency commodity, then look for an ancestor account in a
       currency, but prefer currencies used directly in splits.  Ignore trading
       account splits in this whole process, they don't add any value to this algorithm. */
    for (node = trans->splits; node; node = node->next)
    {
        Split *s = node->data;
        unsigned int curr_weight;

        if (s == NULL || s->acc == NULL) continue;
        if (xaccAccountGetType(s->acc) == ACCT_TYPE_TRADING) continue;
        com_scratch = xaccAccountGetCommodity(s->acc);
        if (com_scratch && gnc_commodity_is_currency(com_scratch))
        {
            curr_weight = 3;
        }
        else
        {
            com_scratch = gnc_account_get_currency_or_parent(s->acc);
            if (com_scratch == NULL) continue;
            curr_weight = 1;
        }
        if ( comlist )
        {
            found = g_slist_find_custom(comlist, com_scratch, commodity_equal);
        }
        if (comlist == NULL || found == NULL)
        {
            CommodityCount *count = g_slice_new0(CommodityCount);
            count->commodity = com_scratch;
            count->count = curr_weight;
            comlist = g_slist_append(comlist, count);
        }
        else
        {
            CommodityCount *count = (CommodityCount*)(found->data);
            count->count += curr_weight;
        }
    }
    found = g_slist_sort( comlist, commodity_compare);

    if ( found && found->data && (((CommodityCount*)(found->data))->commodity != NULL))
    {
        return ((CommodityCount*)(found->data))->commodity;
    }
    /* We didn't find a currency in the current account structure, so try
     * an old one. */
    return xaccTransFindOldCommonCurrency( trans, book );
}

/* ================================================================ */

void
xaccTransScrubCurrency (Transaction *trans)
{
    SplitList *node;
    gnc_commodity *currency;

    if (!trans) return;

    /* If there are any orphaned splits in a transaction, then the
     * this routine will fail.  Therefore, we want to make sure that
     * there are no orphans (splits without parent account).
     */
    xaccTransScrubOrphans (trans);

    currency = xaccTransGetCurrency (trans);
    if (currency && gnc_commodity_is_currency(currency)) return;

    currency = xaccTransFindCommonCurrency (trans, qof_instance_get_book(trans));
    if (currency)
    {
        xaccTransBeginEdit (trans);
        xaccTransSetCurrency (trans, currency);
        xaccTransCommitEdit (trans);
    }
    else
    {
        if (NULL == trans->splits)
        {
            PWARN ("Transaction \"%s\" has no splits in it!", trans->description);
        }
        else
        {
            SplitList *node;
            char guid_str[GUID_ENCODING_LENGTH + 1];
            guid_to_string_buff(xaccTransGetGUID(trans), guid_str);
            PWARN ("no common transaction currency found for trans=\"%s\" (%s);",
                   trans->description, guid_str);

            for (node = trans->splits; node; node = node->next)
            {
                Split *split = node->data;
                if (NULL == split->acc)
                {
                    PWARN (" split=\"%s\" is not in any account!", split->memo);
                }
                else
                {
                    gnc_commodity *currency = xaccAccountGetCommodity(split->acc);
                    PWARN ("setting to split=\"%s\" account=\"%s\" commodity=\"%s\"",
                           split->memo, xaccAccountGetName(split->acc),
                           gnc_commodity_get_mnemonic(currency));

                    xaccTransBeginEdit (trans);
                    xaccTransSetCurrency (trans, currency);
                    xaccTransCommitEdit (trans);
                    return;
                }
            }
        }
        return;
    }

    for (node = trans->splits; node; node = node->next)
    {
        Split *sp = node->data;

        if (!gnc_numeric_equal(xaccSplitGetAmount (sp),
                               xaccSplitGetValue (sp)))
        {
            gnc_commodity *acc_currency;

            acc_currency = sp->acc ? xaccAccountGetCommodity(sp->acc) : NULL;
            if (acc_currency == currency)
            {
                /* This Split needs fixing: The transaction-currency equals
                 * the account-currency/commodity, but the amount/values are
                 * inequal i.e. they still correspond to the security
                 * (amount) and the currency (value). In the new model, the
                 * value is the amount in the account-commodity -- so it
                 * needs to be set to equal the amount (since the
                 * account-currency doesn't exist anymore).
                 *
                 * Note: Nevertheless we lose some information here. Namely,
                 * the information that the 'amount' in 'account-old-security'
                 * was worth 'value' in 'account-old-currency'. Maybe it would
                 * be better to store that information in the price database?
                 * But then, for old currency transactions there is still the
                 * 'other' transaction, which is going to keep that
                 * information. So I don't bother with that here. -- cstim,
                 * 2002/11/20. */

                PWARN ("Adjusted split with mismatched values, desc=\"%s\" memo=\"%s\""
                       " old amount %s %s, new amount %s",
                       trans->description, sp->memo,
                       gnc_num_dbg_to_string (xaccSplitGetAmount(sp)),
                       gnc_commodity_get_mnemonic (currency),
                       gnc_num_dbg_to_string (xaccSplitGetValue(sp)));
                xaccTransBeginEdit (trans);
                xaccSplitSetAmount (sp, xaccSplitGetValue(sp));
                xaccTransCommitEdit (trans);
            }
            /*else
              {
              PINFO ("Ok: Split '%s' Amount %s %s, value %s %s",
              xaccSplitGetMemo (sp),
              gnc_num_dbg_to_string (amount),
              gnc_commodity_get_mnemonic (currency),
              gnc_num_dbg_to_string (value),
              gnc_commodity_get_mnemonic (acc_currency));
              }*/
        }
    }

}

/* ================================================================ */

void
xaccAccountScrubCommodity (Account *account)
{
    gnc_commodity *commodity;

    if (!account) return;
    if (xaccAccountGetType(account) == ACCT_TYPE_ROOT) return;

    commodity = xaccAccountGetCommodity (account);
    if (commodity) return;

    /* Use the 'obsolete' routines to try to figure out what the
     * account commodity should have been. */
    commodity = xaccAccountGetCommodity (account);
    if (commodity)
    {
        xaccAccountSetCommodity (account, commodity);
        return;
    }

    commodity = DxaccAccountGetCurrency (account);
    if (commodity)
    {
        xaccAccountSetCommodity (account, commodity);
        return;
    }

    PERR ("Account \"%s\" does not have a commodity!",
          xaccAccountGetName(account));
}

/* ================================================================ */

/* EFFECTIVE FRIEND FUNCTION declared in qofinstance-p.h */
extern void qof_instance_set_dirty (QofInstance*);

static void
xaccAccountDeleteOldData (Account *account)
{
    if (!account) return;
    xaccAccountBeginEdit (account);
    qof_instance_set_kvp (QOF_INSTANCE (account), NULL, 1, "old-currency");
    qof_instance_set_kvp (QOF_INSTANCE (account), NULL, 1, "old-security");
    qof_instance_set_kvp (QOF_INSTANCE (account), NULL, 1, "old-currency-scu");
    qof_instance_set_kvp (QOF_INSTANCE (account), NULL, 1, "old-security-scu");
    qof_instance_set_dirty (QOF_INSTANCE (account));
    xaccAccountCommitEdit (account);
}

static int
scrub_trans_currency_helper (Transaction *t, gpointer data)
{
    xaccTransScrubCurrency (t);
    return 0;
}

static void
scrub_account_commodity_helper (Account *account, gpointer data)
{
    scrub_depth++;
    xaccAccountScrubCommodity (account);
    xaccAccountDeleteOldData (account);
    scrub_depth--;
}

void
xaccAccountTreeScrubCommodities (Account *acc)
{
    if (!acc) return;
    scrub_depth++;
    xaccAccountTreeForEachTransaction (acc, scrub_trans_currency_helper, NULL);

    scrub_account_commodity_helper (acc, NULL);
    gnc_account_foreach_descendant (acc, scrub_account_commodity_helper, NULL);
    scrub_depth--;
}

/* ================================================================ */

static gboolean
check_quote_source (gnc_commodity *com, gpointer data)
{
    gboolean *commodity_has_quote_src = (gboolean *)data;
    if (com && !gnc_commodity_is_iso(com))
        *commodity_has_quote_src |= gnc_commodity_get_quote_flag(com);
    return TRUE;
}

static void
move_quote_source (Account *account, gpointer data)
{
    gnc_commodity *com;
    gnc_quote_source *quote_source;
    gboolean new_style = GPOINTER_TO_INT(data);
    const char *source, *tz;

    com = xaccAccountGetCommodity(account);
    if (!com)
        return;

    if (!new_style)
    {
        source = dxaccAccountGetPriceSrc(account);
        if (!source || !*source)
            return;
        tz = dxaccAccountGetQuoteTZ(account);

        PINFO("to %8s from %s", gnc_commodity_get_mnemonic(com),
              xaccAccountGetName(account));
        gnc_commodity_set_quote_flag(com, TRUE);
        quote_source = gnc_quote_source_lookup_by_internal(source);
        if (!quote_source)
            quote_source = gnc_quote_source_add_new(source, FALSE);
        gnc_commodity_set_quote_source(com, quote_source);
        gnc_commodity_set_quote_tz(com, tz);
    }

    dxaccAccountSetPriceSrc(account, NULL);
    dxaccAccountSetQuoteTZ(account, NULL);
    return;
}


void
xaccAccountTreeScrubQuoteSources (Account *root, gnc_commodity_table *table)
{
    gboolean new_style = FALSE;
    ENTER(" ");

    if (!root || !table)
    {
        LEAVE("Oops");
        return;
    }
    scrub_depth++;
    gnc_commodity_table_foreach_commodity (table, check_quote_source, &new_style);

    move_quote_source(root, GINT_TO_POINTER(new_style));
    gnc_account_foreach_descendant (root, move_quote_source,
                                    GINT_TO_POINTER(new_style));
    LEAVE("Migration done");
    scrub_depth--;
}

/* ================================================================ */

void
xaccAccountScrubKvp (Account *account)
{
    GValue v = G_VALUE_INIT;
    gchar *str2;

    if (!account) return;
    scrub_depth++;

    qof_instance_get_kvp (QOF_INSTANCE (account), &v, 1, "notes");
    if (G_VALUE_HOLDS_STRING (&v))
    {
        str2 = g_strstrip(g_value_dup_string(&v));
        if (strlen(str2) == 0)
            qof_instance_slot_delete (QOF_INSTANCE (account), "notes");
        g_free(str2);
    }

    qof_instance_get_kvp (QOF_INSTANCE (account), &v, 1, "placeholder");
    if ((G_VALUE_HOLDS_STRING (&v) &&
        strcmp(g_value_get_string (&v), "false") == 0) ||
        (G_VALUE_HOLDS_BOOLEAN (&v) && ! g_value_get_boolean (&v)))
        qof_instance_slot_delete (QOF_INSTANCE (account), "placeholder");

    g_value_unset (&v);
    qof_instance_slot_delete_if_empty (QOF_INSTANCE (account), "hbci");
    scrub_depth--;
}

/* ================================================================ */

void
xaccAccountScrubColorNotSet (QofBook *book)
{
    GValue value_s = G_VALUE_INIT;
    gboolean already_scrubbed;

    // get the run-once value
    qof_instance_get_kvp (QOF_INSTANCE (book), &value_s, 1, "remove-color-not-set-slots");

    already_scrubbed = (G_VALUE_HOLDS_STRING (&value_s) &&
                        !g_strcmp0 (g_value_get_string (&value_s), "true"));
    g_value_unset (&value_s);

    if (already_scrubbed)
        return;
    else
    {
        GValue value_b = G_VALUE_INIT;
        Account *root = gnc_book_get_root_account (book);
        GList *accts = gnc_account_get_descendants_sorted (root);
        GList *ptr;

        for (ptr = accts; ptr; ptr = g_list_next (ptr))
        {
            const gchar *color = xaccAccountGetColor (ptr->data);

            if (g_strcmp0 (color, "Not Set") == 0)
                xaccAccountSetColor (ptr->data, "");
        }
        g_list_free (accts);

        g_value_init (&value_b, G_TYPE_BOOLEAN);
        g_value_set_boolean (&value_b, TRUE);

        // set the run-once value
        qof_instance_set_kvp (QOF_INSTANCE (book),  &value_b, 1, "remove-color-not-set-slots");
        g_value_unset (&value_b);
    }
}

/* ================================================================ */

static Account*
construct_account (Account *root, gnc_commodity *currency, const char *accname,
                   GNCAccountType acctype, gboolean placeholder)
{
    gnc_commodity* root_currency = find_root_currency ();
    Account *acc = xaccMallocAccount(gnc_account_get_book (root));
    xaccAccountBeginEdit (acc);
    if (accname && *accname)
         xaccAccountSetName (acc, accname);
    if (currency || root_currency)
         xaccAccountSetCommodity (acc, currency ? currency : root_currency);
    xaccAccountSetType (acc, acctype);
    xaccAccountSetPlaceholder (acc, placeholder);

    /* Hang the account off the root. */
    gnc_account_append_child (root, acc);
    xaccAccountCommitEdit (acc);
    return acc;
}

static Account*
find_root_currency_account_in_list (GList *acc_list)
{
    gnc_commodity* root_currency = find_root_currency();
    for (GList *node = acc_list; node; node = g_list_next (node))
    {
        Account *acc = GNC_ACCOUNT (node->data);
        gnc_commodity *acc_commodity = NULL;
        if (G_UNLIKELY (!acc)) continue;
        acc_commodity = xaccAccountGetCommodity(acc);
        if (gnc_commodity_equiv (acc_commodity, root_currency))
            return acc;
    }

    return NULL;
}

static Account*
find_account_matching_name_in_list (GList *acc_list, const char* accname)
{
    for (GList* node = acc_list; node; node = g_list_next(node))
    {
        Account *acc = GNC_ACCOUNT (node->data);
        if (G_UNLIKELY (!acc)) continue;
        if (g_strcmp0 (accname, xaccAccountGetName (acc)) == 0)
            return acc;
    }
    return NULL;
}

Account *
xaccScrubUtilityGetOrMakeAccount (Account *root, gnc_commodity * currency,
                                  const char *accname, GNCAccountType acctype,
                                  gboolean placeholder, gboolean checkname)
{
    GList* acc_list;
    Account *acc = NULL;

    g_return_val_if_fail (root, NULL);

    acc_list =
        gnc_account_lookup_by_type_and_commodity (root,
                                                  checkname ? accname : NULL,
                                                  acctype, currency);

    if (!acc_list)
        return construct_account (root, currency, accname,
                                  acctype, placeholder);

    if (g_list_next(acc_list))
    {
        if (!currency)
            acc = find_root_currency_account_in_list (acc_list);

        if (!acc)
            acc = find_account_matching_name_in_list (acc_list, accname);
    }

    if (!acc)
        acc = GNC_ACCOUNT (acc_list->data);

    g_list_free (acc_list);
    return acc;
}

void
xaccTransScrubPostedDate (Transaction *trans)
{
    time64 orig = xaccTransGetDate(trans);
    if(orig == INT64_MAX)
    {
	GDate date = xaccTransGetDatePostedGDate(trans);
	time64 time = gdate_to_time64(date);
	if(time != INT64_MAX)
	{
	    // xaccTransSetDatePostedSecs handles committing the change.
	    xaccTransSetDatePostedSecs(trans, time);
	}
    }
}

/* ==================== END OF FILE ==================== */
