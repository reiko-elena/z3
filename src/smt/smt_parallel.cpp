/*++
Copyright (c) 2020 Microsoft Corporation

Module Name:

    smt_parallel.cpp

Abstract:

    Parallel SMT, portfolio loop specialized to SMT core.

Author:

    nbjorner 2020-01-31

--*/

#include "util/scoped_ptr_vector.h"
#include "ast/ast_util.h"
#include "ast/ast_translation.h"
#include "smt/smt_parallel.h"
#include "smt/smt_lookahead.h"
#include <thread>

namespace smt {
    
    lbool parallel::operator()(expr_ref_vector const& asms) {

        enum par_exception_kind {
            DEFAULT_EX,
            ERROR_EX
        };

        ctx.internalize_assertions();
        scoped_ptr_vector<ast_manager> pms;
        scoped_ptr_vector<context> pctxs;
        vector<expr_ref_vector> pasms;
        ast_manager& m = ctx.m;
        lbool result = l_undef;
        unsigned num_threads = ctx.m_fparams.m_threads;
        flet<unsigned> _nt(ctx.m_fparams.m_threads, 1);
        unsigned finished_id = UINT_MAX;
        std::string        ex_msg;
        par_exception_kind ex_kind = DEFAULT_EX;
        unsigned error_code = 0;
        bool done = false;
        unsigned num_rounds = 0;
        unsigned max_conflicts = ctx.get_fparams().m_threads_max_conflicts;

        for (unsigned i = 0; i < num_threads; ++i) {
            ast_manager* new_m = alloc(ast_manager, m, true);
            pms.push_back(new_m);
            pctxs.push_back(alloc(context, *new_m, ctx.get_fparams(), ctx.get_params())); 
            context& new_ctx = *pctxs.back();
            context::copy(ctx, new_ctx);
            new_ctx.set_random_seed(i + ctx.get_fparams().m_random_seed);
            ast_translation tr(*new_m, m);
            pasms.push_back(tr(asms));
        }

        auto cube = [](context& ctx, expr_ref_vector& lasms, expr_ref& c) {
            lookahead lh(ctx);
            c = lh.choose();
            if (c) lasms.push_back(c);
        };

        obj_hashtable<expr> unit_set;
        expr_ref_vector unit_trail(ctx.m);
        unsigned_vector unit_lim;
        for (unsigned i = 0; i < num_threads; ++i) unit_lim.push_back(0);

        std::function<void(void)> collect_units = [&,this]() {
            for (unsigned i = 0; i < num_threads; ++i) {
                context& pctx = *pctxs[i];
                pctx.pop_to_base_lvl();
                ast_translation tr(pctx.m, ctx.m);
                unsigned sz = pctx.assigned_literals().size();
                for (unsigned j = unit_lim[i]; j < sz; ++j) {
                    literal lit = pctx.assigned_literals()[j];
                    expr_ref e(pctx.bool_var2expr(lit.var()), pctx.m);
                    if (lit.sign()) e = pctx.m.mk_not(e);
                    expr_ref ce(tr(e.get()), ctx.m);
                    if (!unit_set.contains(ce)) {
                        unit_set.insert(ce);
                        unit_trail.push_back(ce);
                    }
                }
            }

            unsigned sz = unit_trail.size();
            for (unsigned i = 0; i < num_threads; ++i) {
                context& pctx = *pctxs[i];
                ast_translation tr(ctx.m, pctx.m);
                for (unsigned j = unit_lim[i]; j < sz; ++j) {
                    expr_ref src(ctx.m), dst(pctx.m);
                    dst = tr(unit_trail.get(j));
                    pctx.assert_expr(dst);
                }
                unit_lim[i] = sz;
            }
        };

        std::mutex mux;

        auto worker_thread = [&](int i) {
            try {
                context& pctx = *pctxs[i];
                ast_manager& pm = *pms[i];
                expr_ref_vector lasms(pasms[i]);
                expr_ref c(pm);

                pctx.get_fparams().m_max_conflicts = max_conflicts;
                if (num_rounds > 0) {
                    cube(pctx, lasms, c);
                }
                IF_VERBOSE(1, verbose_stream() << "(smt.thread " << i; 
                           if (num_rounds > 0) verbose_stream() << " :round " << num_rounds;
                           if (c) verbose_stream() << " :cube: " << c;
                           verbose_stream() << ")\n";);
                lbool r = pctx.check(lasms.size(), lasms.c_ptr());
                
                if (r == l_undef && pctx.m_num_conflicts >= max_conflicts) {
                    return;
                }                

                if (r == l_false && pctx.unsat_core().contains(c)) {
                    pctx.assert_expr(mk_not(mk_and(pctx.unsat_core())));
                    return;
                } 

                bool first = false;
                {
                    std::lock_guard<std::mutex> lock(mux);
                    if (finished_id == UINT_MAX) {
                        finished_id = i;
                        first = true;
                        result = r;
                        done = true;
                    }
                    if (!first) return;
                }

                for (ast_manager* m : pms) {
                    if (m != &pm) m->limit().cancel();
                }

            }
            catch (z3_error & err) {
                error_code = err.error_code();
                ex_kind = ERROR_EX;                
                done = true;
            }
            catch (z3_exception & ex) {
                ex_msg = ex.msg();
                ex_kind = DEFAULT_EX;    
                done = true;
            }
        };

        while (true) {
            vector<std::thread> threads(num_threads);
            for (unsigned i = 0; i < num_threads; ++i) {
                threads[i] = std::thread([&, i]() { worker_thread(i); });
            }
            for (auto & th : threads) {
                th.join();
            }
            if (done) break;

            collect_units();
            ++num_rounds;
            max_conflicts *= 2;
        }

        for (context* c : pctxs) {
            c->collect_statistics(ctx.m_aux_stats);
        }

        if (finished_id == UINT_MAX) {
            switch (ex_kind) {
            case ERROR_EX: throw z3_error(error_code);
            default: throw default_exception(std::move(ex_msg));
            }
        }        

        model_ref mdl;        
        context& pctx = *pctxs[finished_id];
        ast_translation tr(*pms[finished_id], m);
        switch (result) {
        case l_true: 
            pctx.get_model(mdl);
            if (mdl) {
                ctx.m_model = mdl->translate(tr);
            }
            break;
        case l_false:
            for (expr* e : pctx.unsat_core()) 
                ctx.m_unsat_core.push_back(tr(e));
            break;
        default:
            break;
        }                                

        return result;
    }

}
