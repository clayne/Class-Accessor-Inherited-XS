#ifndef __INHERITED_XS_OP_H_
#define __INHERITED_XS_OP_H_

#define OP_UNSTEAL(name) STMT_START {       \
        ++unstolen;                         \
        PL_op->op_ppaddr = PL_ppaddr[name]; \
        return PL_ppaddr[name](aTHX);       \
    } STMT_END                              \

template <AccessorType type, bool is_readonly> static
XSPROTO(CAIXS_entersub_wrapper) {
    dSP;

    CAIXS_accessor<type, is_readonly>(aTHX_ SP, cv, NULL);

    return;
}

#ifdef CAIX_OPTIMIZE_OPMETHOD

template <AccessorType type, int optype, bool is_readonly> static
OP *
CAIXS_opmethod_wrapper(pTHX) {
    dSP;

    SV* self = PL_stack_base + TOPMARK == SP ? (SV*)NULL : *(PL_stack_base + TOPMARK + 1);
    HV* stash = NULL;

    /*
        This block isn't required for the 'goto gotcv' case, but skipping it
        (or swapping those blocks) makes unstealing inside 'goto gotcv' block impossible,
        thus requiring additional check in the fast case, which is to be avoided.
    */
#ifndef GV_CACHE_ONLY
    if (LIKELY(self != NULL)) {
        SvGETMAGIC(self);
#else
    if (LIKELY(self && !SvGMAGICAL(self))) {
        /* SvIsCOW_shared_hash is incompatible with SvGMAGICAL, so skip it completely */
        if (SvIsCOW_shared_hash(self)) {
            stash = gv_stashsv(self, GV_CACHE_ONLY);
        } else
#endif
        if (SvROK(self)) {
            SV* ob = SvRV(self);
            if (SvOBJECT(ob)) stash = SvSTASH(ob);

        } else if (SvPOK(self)) {
            const char* packname = SvPVX_const(self);
            const STRLEN packlen = SvCUR(self);
            const int is_utf8 = SvUTF8(self);

#ifndef GV_CACHE_ONLY
            const HE* const he = (const HE *)hv_common(PL_stashcache, NULL, packname, packlen, is_utf8, 0, NULL, 0);
            if (he) stash = INT2PTR(HV*, SvIV(HeVAL(he)));
            else
#endif
            stash = gv_stashpvn(packname, packlen, is_utf8);
        }
    }

    SV* meth;
    CV* cv = NULL;
    U32 hash;

    if (optype == OP_METHOD) {
        meth = TOPs;
        if (SvROK(meth)) {
            SV* const rmeth = SvRV(meth);
            if (SvTYPE(rmeth) == SVt_PVCV) {
                cv = (CV*)rmeth;
                goto gotcv; /* We don't care about the 'stash' var here */
            }
        }

        hash = 0;

    } else if (optype == OP_METHOD_NAMED) {
        meth = cSVOPx_sv(PL_op);

#ifndef GV_CACHE_ONLY
        hash = SvSHARED_HASH(meth);
#else
        hash = 0;
#endif
    }

    /* SvTYPE check appeared only since 5.22, but execute it for all perls nevertheless */
    if (UNLIKELY(!stash || SvTYPE(stash) != SVt_PVHV)) {
        OP_UNSTEAL(optype);
    }

    HE* he; /* To allow 'goto' to jump over this */
    if ((he = hv_fetch_ent(stash, meth, 0, hash))) {
        GV* gv = (GV*)(HeVAL(he));
        if (isGV(gv) && GvCV(gv) && (!GvCVGEN(gv) || GvCVGEN(gv) == (PL_sub_generation + HvMROMETA(stash)->cache_gen))) {
            cv = GvCV(gv);
        }
    }

    if (UNLIKELY(!cv)) {
        GV* gv = gv_fetchmethod_sv_flags(stash, meth, GV_AUTOLOAD|GV_CROAK);
        assert(gv);

        cv = isGV(gv) ? GvCV(gv) : (CV*)gv;
        assert(cv);
    }

gotcv:
    if (LIKELY((CvXSUB(cv) == (XSUBADDR_t)&CAIXS_entersub_wrapper<type, is_readonly>))) {
        assert(CvISXSUB(cv));

        if (optype == OP_METHOD) {--SP; PUTBACK; }

        CAIXS_accessor<type, is_readonly>(aTHX_ SP, cv, stash);

        return PL_op->op_next->op_next;

    } else {
        /*
            We could also lift off CAIXS_entersub optimization here, but that's a one-time action,
            so let it fail on it's own
        */
        OP_UNSTEAL(optype);
    }
}

#endif /* CAIX_OPTIMIZE_OPMETHOD */

template <AccessorType type, bool is_readonly> static
OP *
CAIXS_entersub(pTHX) {
    dSP;

    CV* sv = (CV*)TOPs;

    if (LIKELY(sv != NULL)) {
        if (UNLIKELY(SvTYPE(sv) != SVt_PVCV)) {
            /* can('acc')->() or (\&acc)->()  */

            if (LIKELY(SvROK(sv))) sv = (CV*)SvRV(sv);
            if (UNLIKELY(SvTYPE(sv) != SVt_PVCV)) OP_UNSTEAL(OP_ENTERSUB);
        }

        /* Some older gcc's can't deduce correct function - have to add explicit cast  */
        if (LIKELY((CvXSUB(sv) == (XSUBADDR_t)&CAIXS_entersub_wrapper<type, is_readonly>))) {
            /*
                Assert against future XPVCV layout change - as for now, xcv_xsub shares space with xcv_root
                which are both pointers, so address check is enough, and there's no need to look into op_flags for CvISXSUB.
            */
            assert(CvISXSUB(sv));

            POPs; PUTBACK;
            CAIXS_accessor<type, is_readonly>(aTHX_ SP, sv, NULL);

            return NORMAL;
        }

    }

    OP_UNSTEAL(OP_ENTERSUB);
}

template <AccessorType type, bool is_readonly> inline
void
CAIXS_install_entersub(pTHX) {
    /*
        Check whether we can replace opcode executor with our own variant. Unfortunatelly, this guards
        only against local changes, not when someone steals PL_ppaddr[OP_ENTERSUB] globally.
        Sorry, Devel::NYTProf.
    */

    OP* op = PL_op;

    if ((op->op_spare & 1) != 1 && op->op_ppaddr == PL_ppaddr[OP_ENTERSUB] && optimize_entersub) {
        op->op_spare |= 1;
        op->op_ppaddr = &CAIXS_entersub<type, is_readonly>;

#ifdef CAIX_OPTIMIZE_OPMETHOD
        OP* methop = cUNOPx(op)->op_first;
        if (LIKELY(methop != NULL)) {   /* Such op can be created by call_sv(G_METHOD_NAMED) */
            while (methop->op_sibling) { methop = methop->op_sibling; }

            if (methop->op_next == op) {
                if (methop->op_type == OP_METHOD_NAMED && methop->op_ppaddr == PL_ppaddr[OP_METHOD_NAMED]) {
                    methop->op_ppaddr = &CAIXS_opmethod_wrapper<type, OP_METHOD_NAMED, is_readonly>;

                } else if (methop->op_type == OP_METHOD && methop->op_ppaddr == PL_ppaddr[OP_METHOD]) {
                    methop->op_ppaddr = &CAIXS_opmethod_wrapper<type, OP_METHOD, is_readonly>;
                }
            }
        }
#endif /* CAIX_OPTIMIZE_OPMETHOD */
    }
}

#endif /* __INHERITED_XS_OP_H_ */