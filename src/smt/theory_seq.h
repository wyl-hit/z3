/*++
Copyright (c) 2011 Microsoft Corporation

Module Name:

    theory_seq.h

Abstract:

    Native theory solver for sequences.

Author:

    Nikolaj Bjorner (nbjorner) 2015-6-12

Revision History:

--*/
#ifndef THEORY_SEQ_H_
#define THEORY_SEQ_H_

#include "smt_theory.h"
#include "seq_decl_plugin.h"
#include "theory_seq_empty.h"
#include "th_rewriter.h"
#include "ast_trail.h"
#include "scoped_vector.h"
#include "scoped_ptr_vector.h"
#include "automaton.h"
#include "seq_rewriter.h"

namespace smt {

    class theory_seq : public theory {
        struct assumption {
            enode* n1, *n2;
            literal lit;
            assumption(enode* n1, enode* n2): n1(n1), n2(n2), lit(null_literal) {}
            assumption(literal lit): n1(0), n2(0), lit(lit) {}
        };
        typedef scoped_dependency_manager<assumption> dependency_manager;
        typedef dependency_manager::dependency dependency;        

        typedef trail_stack<theory_seq> th_trail_stack;
        typedef std::pair<expr*, dependency*> expr_dep;
        typedef obj_map<expr, expr_dep> eqdep_map_t; 

        // cache to track evaluations under equalities
        class eval_cache {
            eqdep_map_t             m_map;
            expr_ref_vector         m_trail;
        public:
            eval_cache(ast_manager& m): m_trail(m) {}
            bool find(expr* v, expr_dep& r) const { return m_map.find(v, r); }
            void insert(expr* v, expr_dep& r) { m_trail.push_back(v); m_trail.push_back(r.first); m_map.insert(v, r); }
            void reset() { m_map.reset(); m_trail.reset(); }
        };
        
        // map from variables to representatives 
        // + a cache for normalization.
        class solution_map {
            enum map_update { INS, DEL };
            ast_manager&                      m;
            dependency_manager&    m_dm;
            eqdep_map_t                       m_map;            
            eval_cache                        m_cache;
            expr_ref_vector                   m_lhs, m_rhs;
            ptr_vector<dependency> m_deps;
            svector<map_update>               m_updates;
            unsigned_vector                   m_limit;

            void add_trail(map_update op, expr* l, expr* r, dependency* d);
        public:
            solution_map(ast_manager& m, dependency_manager& dm): 
                m(m),  m_dm(dm), m_cache(m), m_lhs(m), m_rhs(m) {}
            bool  empty() const { return m_map.empty(); }
            void  update(expr* e, expr* r, dependency* d);
            void  add_cache(expr* v, expr_dep& r) { m_cache.insert(v, r); }
            bool  find_cache(expr* v, expr_dep& r) { return m_cache.find(v, r); }
            expr* find(expr* e, dependency*& d);
            expr* find(expr* e);
            bool  is_root(expr* e) const;
            void  cache(expr* e, expr* r, dependency* d);
            void  reset_cache() { m_cache.reset(); }
            void  push_scope() { m_limit.push_back(m_updates.size()); }
            void  pop_scope(unsigned num_scopes);
            void  display(std::ostream& out) const;
        };

        // Table of current disequalities
        class exclusion_table {
            typedef obj_pair_hashtable<expr, expr> table_t;
            ast_manager&                      m;
            table_t                           m_table;
            expr_ref_vector                   m_lhs, m_rhs;
            unsigned_vector                   m_limit;
        public:
            exclusion_table(ast_manager& m): m(m), m_lhs(m), m_rhs(m) {}
            ~exclusion_table() { }
            bool empty() const { return m_table.empty(); }
            void update(expr* e, expr* r);
            bool contains(expr* e, expr* r) const;
            void push_scope() { m_limit.push_back(m_lhs.size()); }
            void pop_scope(unsigned num_scopes);
            void display(std::ostream& out) const;            
        };

        // Asserted or derived equality with dependencies
        class eq {
            unsigned         m_id;
            expr_ref_vector  m_lhs;
            expr_ref_vector  m_rhs;
            dependency*      m_dep;
        public:
            
            eq(unsigned id, expr_ref_vector& l, expr_ref_vector& r, dependency* d):
                m_id(id), m_lhs(l), m_rhs(r), m_dep(d) {}
            eq(eq const& other): m_id(other.m_id), m_lhs(other.m_lhs), m_rhs(other.m_rhs), m_dep(other.m_dep) {}
            eq& operator=(eq const& other) {
                if (this != &other) {
                    m_lhs.reset(); 
                    m_rhs.reset();
                    m_lhs.append(other.m_lhs); 
                    m_rhs.append(other.m_rhs); 
                    m_dep = other.m_dep;
                    m_id = other.m_id;
                } 
                return *this; 
            }
            expr_ref_vector const& ls() const { return m_lhs; }
            expr_ref_vector const& rs() const { return m_rhs; }
            dependency* dep() const { return m_dep; }
            unsigned id() const { return m_id; }
        };

        eq mk_eqdep(expr* l, expr* r, dependency* dep) {
            expr_ref_vector ls(m), rs(m);
            m_util.str.get_concat(l, ls);
            m_util.str.get_concat(r, rs);
            return eq(m_eq_id++, ls, rs, dep);
        }


        class ne {            
            vector<expr_ref_vector>  m_lhs;
            vector<expr_ref_vector>  m_rhs;
            literal_vector           m_lits;
            dependency*              m_dep;
        public:
            ne(expr_ref const& l, expr_ref const& r, dependency* dep):
                m_dep(dep) {
                expr_ref_vector ls(l.get_manager()); ls.push_back(l);
                expr_ref_vector rs(r.get_manager()); rs.push_back(r);
                    m_lhs.push_back(ls);
                    m_rhs.push_back(rs);
                }

            ne(vector<expr_ref_vector> const& l, vector<expr_ref_vector> const& r, literal_vector const& lits, dependency* dep):
                m_lhs(l),
                m_rhs(r),
                m_lits(lits),
                m_dep(dep) {}

            ne(ne const& other): 
                m_lhs(other.m_lhs), m_rhs(other.m_rhs), m_lits(other.m_lits), m_dep(other.m_dep) {}

            ne& operator=(ne const& other) { 
                if (this != &other) {
                    m_lhs.reset();  m_lhs.append(other.m_lhs);
                    m_rhs.reset();  m_rhs.append(other.m_rhs); 
                    m_lits.reset(); m_lits.append(other.m_lits); 
                    m_dep = other.m_dep; 
                }
                return *this; 
            }            
            vector<expr_ref_vector> const& ls() const { return m_lhs; }
            vector<expr_ref_vector> const& rs() const { return m_rhs; }
            expr_ref_vector const& ls(unsigned i) const { return m_lhs[i]; }
            expr_ref_vector const& rs(unsigned i) const { return m_rhs[i]; }
            literal_vector const& lits() const { return m_lits; }
            literal lits(unsigned i) const { return m_lits[i]; }
            dependency* dep() const { return m_dep; }
        };

        class apply {
        public:
            virtual ~apply() {}
            virtual void operator()(theory_seq& th) = 0;
        };

        class replay_length_coherence : public apply {
            expr_ref m_e;
        public:
            replay_length_coherence(ast_manager& m, expr* e) : m_e(e, m) {}
            virtual void operator()(theory_seq& th) {
                th.check_length_coherence(m_e);
                m_e.reset();
            }
        };

        class replay_axiom : public apply {
            expr_ref m_e;
        public:
            replay_axiom(ast_manager& m, expr* e) : m_e(e, m) {}
            virtual void operator()(theory_seq& th) {
                th.enque_axiom(m_e);
                m_e.reset();
            }
        };

        class push_replay : public trail<theory_seq> {
            apply* m_apply;
        public:
            push_replay(apply* app): m_apply(app) {}
            virtual void undo(theory_seq& th) {
                th.m_replay.push_back(m_apply);
            }
        };

        class pop_branch : public trail<theory_seq> {
            unsigned k;
        public:
            pop_branch(unsigned k): k(k) {}
            virtual void undo(theory_seq& th) {
                th.m_branch_start.erase(k);
            }
        };

        void erase_index(unsigned idx, unsigned i);

        struct stats {
            stats() { reset(); }
            void reset() { memset(this, 0, sizeof(stats)); }
            unsigned m_num_splits;
            unsigned m_num_reductions;
            unsigned m_propagate_automata;
            unsigned m_check_length_coherence;
            unsigned m_branch_variable;
            unsigned m_solve_nqs;
            unsigned m_solve_eqs;
            unsigned m_add_axiom;
        };
        ast_manager&               m;
        dependency_manager         m_dm;
        solution_map               m_rep;        // unification representative.
        scoped_vector<eq>          m_eqs;        // set of current equations.
        scoped_vector<ne>          m_nqs;        // set of current disequalities.
        unsigned                   m_eq_id;

        seq_factory*               m_factory;    // value factory
        exclusion_table            m_exclude;    // set of asserted disequalities.
        expr_ref_vector            m_axioms;     // list of axioms to add.
        obj_hashtable<expr>        m_axiom_set;
        unsigned                   m_axioms_head; // index of first axiom to add.
        bool            m_incomplete;             // is the solver (clearly) incomplete for the fragment.
        obj_hashtable<expr> m_length;             // is length applied
        scoped_ptr_vector<apply> m_replay;        // set of actions to replay
        model_generator* m_mg;
        th_rewriter      m_rewrite;
        seq_rewriter     m_seq_rewrite;
        seq_util         m_util;
        arith_util       m_autil;
        th_trail_stack   m_trail_stack;
        stats            m_stats;
        symbol           m_prefix, m_suffix, m_contains_left, m_contains_right, m_accept, m_reject;
        symbol           m_tail, m_nth, m_seq_first, m_seq_last, m_indexof_left, m_indexof_right, m_aut_step;
        symbol           m_extract_prefix, m_at_left, m_at_right;
        ptr_vector<expr> m_todo;
        expr_ref_vector  m_ls, m_rs, m_lhs, m_rhs;

        // maintain automata with regular expressions.
        scoped_ptr_vector<eautomaton>  m_automata;
        obj_map<expr, eautomaton*>     m_re2aut;

        // queue of asserted atoms
        ptr_vector<expr>               m_atoms;
        unsigned_vector                m_atoms_lim;
        unsigned                       m_atoms_qhead;
        bool                           m_new_solution;     // new solution added
        bool                           m_new_propagation;  // new propagation to core

        virtual final_check_status final_check_eh();
        virtual bool internalize_atom(app* atom, bool) { return internalize_term(atom); }
        virtual bool internalize_term(app*);
        virtual void internalize_eq_eh(app * atom, bool_var v) {}
        virtual void new_eq_eh(theory_var, theory_var);
        virtual void new_diseq_eh(theory_var, theory_var);
        virtual void assign_eh(bool_var v, bool is_true);        
        virtual bool can_propagate();
        virtual void propagate();
        virtual void push_scope_eh();
        virtual void pop_scope_eh(unsigned num_scopes);
        virtual void restart_eh();
        virtual void relevant_eh(app* n);
        virtual theory* mk_fresh(context* new_ctx) { return alloc(theory_seq, new_ctx->get_manager()); }
        virtual char const * get_name() const { return "seq"; }
        virtual theory_var mk_var(enode* n);
        virtual void apply_sort_cnstr(enode* n, sort* s);
        virtual void display(std::ostream & out) const;        
        virtual void collect_statistics(::statistics & st) const;
        virtual model_value_proc * mk_value(enode * n, model_generator & mg);
        virtual void init_model(model_generator & mg);

        // final check 
        bool simplify_and_solve_eqs();   // solve unitary equalities
        bool branch_variable();          // branch on a variable
        bool split_variable();           // split a variable
        bool is_solved(); 
        bool check_length_coherence();
        bool check_length_coherence(expr* e);
        bool propagate_length_coherence(expr* e);  

        bool solve_eqs(unsigned start);
        bool solve_eq(expr_ref_vector const& l, expr_ref_vector const& r, dependency* dep);
        bool simplify_eq(expr_ref_vector& l, expr_ref_vector& r, dependency* dep);
        bool solve_unit_eq(expr* l, expr* r, dependency* dep);
        bool solve_unit_eq(expr_ref_vector const& l, expr_ref_vector const& r, dependency* dep);
        bool is_binary_eq(expr_ref_vector const& l, expr_ref_vector const& r, expr*& x, ptr_vector<expr>& xunits, ptr_vector<expr>& yunits, expr*& y);
        bool solve_binary_eq(expr_ref_vector const& l, expr_ref_vector const& r, dependency* dep);
        bool propagate_max_length(expr* l, expr* r, dependency* dep);

        expr_ref mk_empty(sort* s) { return expr_ref(m_util.str.mk_empty(s), m); }
        expr_ref mk_concat(unsigned n, expr*const* es) { return expr_ref(m_util.str.mk_concat(n, es), m); }
        expr_ref mk_concat(expr_ref_vector const& es, sort* s) { if (es.empty()) return mk_empty(s); return mk_concat(es.size(), es.c_ptr()); }
        expr_ref mk_concat(expr* e1, expr* e2) { return expr_ref(m_util.str.mk_concat(e1, e2), m); }
        expr_ref mk_concat(expr* e1, expr* e2, expr* e3) { return expr_ref(m_util.str.mk_concat(e1, e2, e3), m); }
        bool solve_nqs(unsigned i);
        bool solve_ne(unsigned i);

        // asserting consequences
        void linearize(dependency* dep, enode_pair_vector& eqs, literal_vector& lits) const;
        void propagate_lit(dependency* dep, literal lit) { propagate_lit(dep, 0, 0, lit); }
        void propagate_lit(dependency* dep, unsigned n, literal const* lits, literal lit);
        void propagate_eq(dependency* dep, enode* n1, enode* n2);
        void propagate_eq(literal lit, expr* e1, expr* e2, bool add_to_eqs = false);
        void set_conflict(dependency* dep, literal_vector const& lits = literal_vector());

        u_map<unsigned> m_branch_start;
        void insert_branch_start(unsigned k, unsigned s);
        unsigned find_branch_start(unsigned k);
        bool find_branch_candidate(unsigned& start, dependency* dep, expr_ref_vector const& ls, expr_ref_vector const& rs);
        bool can_be_equal(unsigned szl, expr* const* ls, unsigned szr, expr* const* rs) const;
        lbool assume_equality(expr* l, expr* r);

        // variable solving utilities
        bool occurs(expr* a, expr* b);
        bool occurs(expr* a, expr_ref_vector const& b);
        bool is_var(expr* b);
        bool add_solution(expr* l, expr* r, dependency* dep);
        bool is_nth(expr* a) const;
        bool is_tail(expr* a, expr*& s, unsigned& idx) const;
        expr_ref mk_nth(expr* s, expr* idx);
        expr_ref mk_last(expr* e);
        expr_ref canonize(expr* e, dependency*& eqs);
        bool canonize(expr* e, expr_ref_vector& es, dependency*& eqs);
        bool canonize(expr_ref_vector const& es, expr_ref_vector& result, dependency*& eqs);
        expr_ref expand(expr* e, dependency*& eqs);
        void add_dependency(dependency*& dep, enode* a, enode* b);

        void get_concat(expr* e, ptr_vector<expr>& concats);
    
        // terms whose meaning are encoded using axioms.
        void enque_axiom(expr* e);
        void deque_axiom(expr* e);
        void add_axiom(literal l1, literal l2 = null_literal, literal l3 = null_literal, literal l4 = null_literal);        
        void add_indexof_axiom(expr* e);
        void add_replace_axiom(expr* e);
        void add_extract_axiom(expr* e);
        void add_length_axiom(expr* n);

        bool has_length(expr *e) const { return m_length.contains(e); }
        void add_length(expr* e);
        void enforce_length(enode* n);
        void enforce_length_coherence(enode* n1, enode* n2);

        void add_elim_string_axiom(expr* n);
        void add_at_axiom(expr* n);
        void add_in_re_axiom(expr* n);
        literal mk_literal(expr* n);
        literal mk_eq_empty(expr* n);
        literal mk_equals(expr* a, expr* b);
        void tightest_prefix(expr* s, expr* x, literal lit, literal lit2 = null_literal);
        expr_ref mk_sub(expr* a, expr* b);
        enode* ensure_enode(expr* a);

        // arithmetic integration
        bool lower_bound(expr* s, rational& lo);
        bool upper_bound(expr* s, rational& hi);
        bool get_length(expr* s, rational& val);

        void mk_decompose(expr* e, expr_ref& head, expr_ref& tail);
        expr_ref mk_skolem(symbol const& s, expr* e1, expr* e2 = 0, expr* e3 = 0, sort* range = 0);
        bool is_skolem(symbol const& s, expr* e) const;

        void set_incomplete(app* term);

        // automata utilities
        void propagate_in_re(expr* n, bool is_true);
        eautomaton* get_automaton(expr* e);
        literal mk_accept(expr* s, expr* idx, expr* re, expr* state);
        literal mk_accept(expr* s, expr* idx, expr* re, unsigned i) { return mk_accept(s, idx, re, m_autil.mk_int(i)); }
        bool is_accept(expr* acc) const {  return is_skolem(m_accept, acc); }
        bool is_accept(expr* acc, expr*& s, expr*& idx, expr*& re, unsigned& i, eautomaton*& aut) {
            return is_acc_rej(m_accept, acc, s, idx, re, i, aut);
        }
        literal mk_reject(expr* s, expr* idx, expr* re, expr* state);
        literal mk_reject(expr* s, expr* idx, expr* re, unsigned i) { return mk_reject(s, idx, re, m_autil.mk_int(i)); }
        bool is_reject(expr* rej) const {  return is_skolem(m_reject, rej); }
        bool is_reject(expr* rej, expr*& s, expr*& idx, expr*& re, unsigned& i, eautomaton*& aut) {
            return is_acc_rej(m_reject, rej, s, idx, re, i, aut);
        }
        bool is_acc_rej(symbol const& ar, expr* e, expr*& s, expr*& idx, expr*& re, unsigned& i, eautomaton*& aut);
        expr_ref mk_step(expr* s, expr* tail, expr* re, unsigned i, unsigned j, expr* t);
        bool is_step(expr* e, expr*& s, expr*& tail, expr*& re, expr*& i, expr*& j, expr*& t) const;
        bool is_step(expr* e) const;
        void propagate_step(literal lit, expr* n);
        bool add_reject2reject(expr* rej);
        bool add_accept2step(expr* acc);       
        bool add_step2accept(expr* step);
        bool add_prefix2prefix(expr* e);
        bool add_suffix2suffix(expr* e);
        bool add_contains2contains(expr* e);
        void ensure_nth(literal lit, expr* s, expr* idx);
        bool canonizes(bool sign, expr* e);
        void propagate_non_empty(literal lit, expr* s);
        void propagate_is_conc(expr* e, expr* conc);
        void propagate_acc_rej_length(literal lit, expr* acc_rej);
        bool propagate_automata();
        void add_atom(expr* e);
        void new_eq_eh(dependency* dep, enode* n1, enode* n2);

        // diagnostics
        void display_equations(std::ostream& out) const;
        void display_disequations(std::ostream& out) const;
        void display_disequation(std::ostream& out, ne const& e) const;
        void display_deps(std::ostream& out, dependency* deps) const;
    public:
        theory_seq(ast_manager& m);
        virtual ~theory_seq();

        // model building
        app* mk_value(app* a);

    };
};

#endif /* THEORY_SEQ_H_ */

