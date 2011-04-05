// queryutil.h

/*    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#pragma once

#include "jsobj.h"
#include "indexkey.h"

namespace mongo {

    /**
     * One side of an interval of valid BSONElements, specified by a value and a
     * boolean indicating whether the interval includes the value.
     */
    struct FieldBound {
        BSONElement _bound;
        bool _inclusive;
        bool operator==( const FieldBound &other ) const {
            return _bound.woCompare( other._bound ) == 0 &&
                   _inclusive == other._inclusive;
        }
        void flipInclusive() { _inclusive = !_inclusive; }
    };

    /** A closed interval composed of a lower and an upper FieldBound. */
    struct FieldInterval {
        FieldInterval() : _cachedEquality( -1 ) {}
        FieldInterval( const BSONElement& e ) : _cachedEquality( -1 ) {
            _lower._bound = _upper._bound = e;
            _lower._inclusive = _upper._inclusive = true;
        }
        FieldBound _lower;
        FieldBound _upper;
        /** @return true iff no single element can be contained in the interval. */
        bool strictValid() const {
            int cmp = _lower._bound.woCompare( _upper._bound, false );
            return ( cmp < 0 || ( cmp == 0 && _lower._inclusive && _upper._inclusive ) );
        }
        /** @return true iff the interval is an equality constraint. */
        bool equality() const {
            if ( _cachedEquality == -1 ) {
                _cachedEquality = ( _lower._inclusive && _upper._inclusive && _lower._bound.woCompare( _upper._bound, false ) == 0 );
            }
            return _cachedEquality;
        }
        mutable int _cachedEquality;
    };

    /**
     * An ordered list of FieldIntervals expressing constraints on valid
     * BSONElement values for a field.
     */
    class FieldRange {
    public:
        FieldRange( const BSONElement &e = BSONObj().firstElement() , bool isNot=false , bool optimize=true );

        /** @return Range intersection with 'other'. */
        const FieldRange &operator&=( const FieldRange &other );
        /** @return Range union with 'other'. */
        const FieldRange &operator|=( const FieldRange &other );
        /** @return Range of elements elements included in 'this' but not 'other'. */
        const FieldRange &operator-=( const FieldRange &other );
        /** @return true iff this range is a subset of 'other'. */
        bool operator<=( const FieldRange &other );

        /**
         * If there are any valid values for this range, the extreme values can
         * be extracted.
         */
        
        BSONElement min() const { assert( !empty() ); return _intervals[ 0 ]._lower._bound; }
        BSONElement max() const { assert( !empty() ); return _intervals[ _intervals.size() - 1 ]._upper._bound; }
        bool minInclusive() const { assert( !empty() ); return _intervals[ 0 ]._lower._inclusive; }
        bool maxInclusive() const { assert( !empty() ); return _intervals[ _intervals.size() - 1 ]._upper._inclusive; }

        /** @return true iff this range expresses a single equality interval. */
        bool equality() const {
            return
                !empty() &&
                min().woCompare( max(), false ) == 0 &&
                maxInclusive() &&
                minInclusive();
        }
        /** @return true if all the intervals for this range are equalities */
        bool inQuery() const {
            if ( equality() ) {
                return true;
            }
            for( vector< FieldInterval >::const_iterator i = _intervals.begin(); i != _intervals.end(); ++i ) {
                if ( !i->equality() ) {
                    return false;
                }
            }
            return true;
        }
        /**
         * @return true iff this range does not include every BSONElement
         *
         * TODO Assumes intervals are contiguous and minKey/maxKey will not be
         * matched against.
         */
        bool nontrivial() const {
            return
                ! empty() &&
                ( _intervals.size() != 1 ||
                  minKey.firstElement().woCompare( min(), false ) != 0 ||
                  maxKey.firstElement().woCompare( max(), false ) != 0 );
        }
        /** @return true iff this range matches no BSONElements. */
        bool empty() const { return _intervals.empty(); }
        
        /** Empty the range so it matches no BSONElements. */
        void makeEmpty() { _intervals.clear(); }
        const vector< FieldInterval > &intervals() const { return _intervals; }
        string getSpecial() const { return _special; }
        /** Make component intervals noninclusive. */
        void setExclusiveBounds() {
            for( vector< FieldInterval >::iterator i = _intervals.begin(); i != _intervals.end(); ++i ) {
                i->_lower._inclusive = false;
                i->_upper._inclusive = false;
            }
        }
        /**
         * Constructs a range where all FieldIntervals and FieldBounds are in
         * the opposite order of the current range.
         * NOTE the resulting intervals may not be strictValid().
         */
        void reverse( FieldRange &ret ) const {
            assert( _special.empty() );
            ret._intervals.clear();
            ret._objData = _objData;
            for( vector< FieldInterval >::const_reverse_iterator i = _intervals.rbegin(); i != _intervals.rend(); ++i ) {
                FieldInterval fi;
                fi._lower = i->_upper;
                fi._upper = i->_lower;
                ret._intervals.push_back( fi );
            }
        }
    private:
        BSONObj addObj( const BSONObj &o );
        void finishOperation( const vector< FieldInterval > &newIntervals, const FieldRange &other );
        vector< FieldInterval > _intervals;
        /** BSONObj references to keep our BSONElement memory valid. */
        vector< BSONObj > _objData;
        string _special;
    };

    /**
     * Implements query pattern matching, used to determine if a query is
     * similar to an earlier query and should use the same plan.
     *
     * Two queries will generate the same QueryPattern, and therefore match each
     * other, if their fields have the same Types and they have the same sort
     * spec.
     */
    class QueryPattern {
    public:
        friend class FieldRangeSet;
        enum Type {
            Equality,
            LowerBound,
            UpperBound,
            UpperAndLowerBound
        };
        bool operator<( const QueryPattern &other ) const {
            map< string, Type >::const_iterator i = _fieldTypes.begin();
            map< string, Type >::const_iterator j = other._fieldTypes.begin();
            while( i != _fieldTypes.end() ) {
                if ( j == other._fieldTypes.end() )
                    return false;
                if ( i->first < j->first )
                    return true;
                else if ( i->first > j->first )
                    return false;
                if ( i->second < j->second )
                    return true;
                else if ( i->second > j->second )
                    return false;
                ++i;
                ++j;
            }
            if ( j != other._fieldTypes.end() )
                return true;
            return _sort.woCompare( other._sort ) < 0;
        }
        // for testing only, speed unimportant
        bool operator==( const QueryPattern &other ) const {
            bool less = operator<( other );
            bool more = other.operator<( *this );
            assert( !( less && more ) );
            return !( less || more );
        }
        bool operator!=( const QueryPattern &other ) const {
            return !operator==( other );
        }
    private:
        QueryPattern() {}
        void setSort( const BSONObj sort ) {
            _sort = normalizeSort( sort );
        }
        BSONObj static normalizeSort( const BSONObj &spec ) {
            if ( spec.isEmpty() )
                return spec;
            int direction = ( spec.firstElement().number() >= 0 ) ? 1 : -1;
            BSONObjIterator i( spec );
            BSONObjBuilder b;
            while( i.moreWithEOO() ) {
                BSONElement e = i.next();
                if ( e.eoo() )
                    break;
                b.append( e.fieldName(), direction * ( ( e.number() >= 0 ) ? -1 : 1 ) );
            }
            return b.obj();
        }
        map< string, Type > _fieldTypes;
        BSONObj _sort;
    };

    /**
     * A BoundList contains intervals specified by inclusive start
     * and end bounds.  The intervals should be nonoverlapping and occur in
     * the specified direction of traversal.  For example, given a simple index {i:1}
     * and direction +1, one valid BoundList is: (1, 2); (4, 6).  The same BoundList
     * would be valid for index {i:-1} with direction -1.
     */
    typedef vector< pair< BSONObj, BSONObj > > BoundList;

    /**
     * A set of FieldRanges determined from constraints on the fields of a query,
     * that may be used to determine index bounds.
     */
    class FieldRangeSet {
    public:
        friend class FieldRangeOrSet;
        friend class FieldRangeVector;
        FieldRangeSet( const char *ns, const BSONObj &query , bool optimize=true );
        
        /** @return true if there is a nontrivial range for the given field. */
        bool hasRange( const char *fieldName ) const {
            map< string, FieldRange >::const_iterator f = _ranges.find( fieldName );
            return f != _ranges.end();
        }
        /** @return range for the given field. */
        const FieldRange &range( const char *fieldName ) const {
            map< string, FieldRange >::const_iterator f = _ranges.find( fieldName );
            if ( f == _ranges.end() )
                return trivialRange();
            return f->second;
        }
        /** @return range for the given field. */
        FieldRange &range( const char *fieldName ) {
            map< string, FieldRange >::iterator f = _ranges.find( fieldName );
            if ( f == _ranges.end() )
                return trivialRange();
            return f->second;
        }
        /** @return the number of nontrivial ranges. */
        int nNontrivialRanges() const {
            int count = 0;
            for( map< string, FieldRange >::const_iterator i = _ranges.begin(); i != _ranges.end(); ++i ) {
                if ( i->second.nontrivial() )
                    ++count;
            }
            return count;
        }
        /**
         * @return true iff no FieldRanges are empty.
         */
        bool matchPossible() const {
            for( map< string, FieldRange >::const_iterator i = _ranges.begin(); i != _ranges.end(); ++i )
                if ( i->second.empty() )
                    return false;
            return true;
        }
        
        const char *ns() const { return _ns; }
        
        /**
         * @return a simplified query from the extreme values of the nontrivial
         * fields.
         * @param fields If specified, the fields of the returned object are
         * ordered to match those of 'fields'.
         */
        BSONObj simplifiedQuery( const BSONObj &fields = BSONObj() ) const;
        
        QueryPattern pattern( const BSONObj &sort = BSONObj() ) const;
        string getSpecial() const;

        /**
         * @return a FieldRangeSet approximation of the documents in 'this' but
         * not in 'other'.  The approximation will be a superset of the documents
         * in 'this' but not 'other'.
         *
         * Btree scanning for a multidimentional key range will yield a
         * multidimensional box.  The idea here is that if an 'other'
         * multidimensional box contains the current box we don't have to scan
         * the current box.  If the 'other' box contains the current box in
         * all dimensions but one, we can safely subtract the values of 'other'
         * along that one dimension from the values for the current box on the
         * same dimension.  In other situations, subtracting the 'other'
         * box from the current box yields a result that is not a box (but
         * rather can be expressed as a union of boxes).  We don't support
         * such splitting currently in calculating index ranges.  Note that
         * where I have said 'box' above, I actually mean sets of boxes because
         * a field range can consist of multiple intervals.
         */
        const FieldRangeSet &operator-=( const FieldRangeSet &other ) {
            int nUnincluded = 0;
            string unincludedKey;
            map< string, FieldRange >::iterator i = _ranges.begin();
            map< string, FieldRange >::const_iterator j = other._ranges.begin();
            while( nUnincluded < 2 && i != _ranges.end() && j != other._ranges.end() ) {
                int cmp = i->first.compare( j->first );
                if ( cmp == 0 ) {
                    if ( i->second <= j->second ) {
                        // nothing
                    }
                    else {
                        ++nUnincluded;
                        unincludedKey = i->first;
                    }
                    ++i;
                    ++j;
                }
                else if ( cmp < 0 ) {
                    ++i;
                }
                else {
                    // other has a bound we don't, nothing can be done
                    return *this;
                }
            }
            if ( j != other._ranges.end() ) {
                // other has a bound we don't, nothing can be done
                return *this;
            }
            if ( nUnincluded > 1 ) {
                return *this;
            }
            if ( nUnincluded == 0 ) {
                makeEmpty();
                return *this;
            }
            // nUnincluded == 1
            _ranges[ unincludedKey ] -= other._ranges[ unincludedKey ];
            appendQueries( other );
            return *this;
        }
        /** @return intersection of 'this' with 'other'. */
        const FieldRangeSet &operator&=( const FieldRangeSet &other ) {
            map< string, FieldRange >::iterator i = _ranges.begin();
            map< string, FieldRange >::const_iterator j = other._ranges.begin();
            while( i != _ranges.end() && j != other._ranges.end() ) {
                int cmp = i->first.compare( j->first );
                if ( cmp == 0 ) {
                    i->second &= j->second;
                    ++i;
                    ++j;
                }
                else if ( cmp < 0 ) {
                    ++i;
                }
                else {
                    _ranges[ j->first ] = j->second;
                    ++j;
                }
            }
            while( j != other._ranges.end() ) {
                _ranges[ j->first ] = j->second;
                ++j;
            }
            appendQueries( other );
            return *this;
        }
        
        /**
         * @return an ordered list of bounds generated using an index key pattern
         * and traversal direction.
         *
         * NOTE This function is deprecated in the query optimizer and only
         * currently used by the sharding code.
         */
        BoundList indexBounds( const BSONObj &keyPattern, int direction ) const;

        /**
         * @return - A new FieldRangeSet based on this FieldRangeSet, but with only
         * a subset of the fields.
         * @param fields - Only fields which are represented as field names in this object
         * will be included in the returned FieldRangeSet.
         */
        FieldRangeSet *subset( const BSONObj &fields ) const;
    private:
        void appendQueries( const FieldRangeSet &other ) {
            for( vector< BSONObj >::const_iterator i = other._queries.begin(); i != other._queries.end(); ++i ) {
                _queries.push_back( *i );
            }
        }
        void makeEmpty() {
            for( map< string, FieldRange >::iterator i = _ranges.begin(); i != _ranges.end(); ++i ) {
                i->second.makeEmpty();
            }
        }
        void processQueryField( const BSONElement &e, bool optimize );
        void processOpElement( const char *fieldName, const BSONElement &f, bool isNot, bool optimize );
        static FieldRange *__trivialRange;
        static FieldRange &trivialRange();
        mutable map< string, FieldRange > _ranges;
        const char *_ns;
        // make sure memory for FieldRange BSONElements is owned
        vector< BSONObj > _queries;
    };

    class IndexSpec;

    /**
     * An ordered list of fields and their FieldRanges, correspoinding to valid
     * index keys for a given index spec.
     */
    class FieldRangeVector {
    public:
        /**
         * @param frs The valid ranges for all fields, as defined by the query spec
         * @param indexSpec The index spec (key pattern and info)
         * @param direction The direction of index traversal
         */
        FieldRangeVector( const FieldRangeSet &frs, const IndexSpec &indexSpec, int direction )
            :_indexSpec( indexSpec ), _direction( direction >= 0 ? 1 : -1 ) {
            _queries = frs._queries;
            BSONObjIterator i( _indexSpec.keyPattern );
            while( i.more() ) {
                BSONElement e = i.next();
                int number = (int) e.number(); // returns 0.0 if not numeric
                bool forward = ( ( number >= 0 ? 1 : -1 ) * ( direction >= 0 ? 1 : -1 ) > 0 );
                if ( forward ) {
                    _ranges.push_back( frs.range( e.fieldName() ) );
                }
                else {
                    _ranges.push_back( FieldRange() );
                    frs.range( e.fieldName() ).reverse( _ranges.back() );
                }
                assert( !_ranges.back().empty() );
            }
            uassert( 13385, "combinatorial limit of $in partitioning of result set exceeded", size() < 1000000 );
        }

        /** @return the number of index ranges represented by 'this' */
        long long size() {
            long long ret = 1;
            for( vector< FieldRange >::const_iterator i = _ranges.begin(); i != _ranges.end(); ++i ) {
                ret *= i->intervals().size();
            }
            return ret;
        }
        /** @return starting point for an index traversal. */
        BSONObj startKey() const {
            BSONObjBuilder b;
            for( vector< FieldRange >::const_iterator i = _ranges.begin(); i != _ranges.end(); ++i ) {
                const FieldInterval &fi = i->intervals().front();
                b.appendAs( fi._lower._bound, "" );
            }
            return b.obj();
        }
        /** @return end point for an index traversal. */
        BSONObj endKey() const {
            BSONObjBuilder b;
            for( vector< FieldRange >::const_iterator i = _ranges.begin(); i != _ranges.end(); ++i ) {
                const FieldInterval &fi = i->intervals().back();
                b.appendAs( fi._upper._bound, "" );
            }
            return b.obj();
        }
        /** @return a client readable representation of 'this' */
        BSONObj obj() const {
            BSONObjBuilder b;
            BSONObjIterator k( _indexSpec.keyPattern );
            for( int i = 0; i < (int)_ranges.size(); ++i ) {
                BSONArrayBuilder a( b.subarrayStart( k.next().fieldName() ) );
                for( vector< FieldInterval >::const_iterator j = _ranges[ i ].intervals().begin();
                        j != _ranges[ i ].intervals().end(); ++j ) {
                    a << BSONArray( BSON_ARRAY( j->_lower._bound << j->_upper._bound ).clientReadable() );
                }
                a.done();
            }
            return b.obj();
        }
        
        /**
         * @return true iff the provided document matches valid ranges on all
         * of this FieldRangeVector's fields, which is the case iff this document
         * would be returned while scanning the index corresponding to this
         * FieldRangeVector.  This function is used for $or clause deduping.
         */
        bool matches( const BSONObj &obj ) const;
        
        /**
         * Helper class for iterating through an ordered representation of keys
         * to find those keys that match a specified FieldRangeVector.
         */
        class Iterator {
        public:
            Iterator( const FieldRangeVector &v ) : _v( v ), _i( _v._ranges.size(), -1 ), _cmp( _v._ranges.size(), 0 ), _inc( _v._ranges.size(), false ), _after() {
            }
            static BSONObj minObject() {
                BSONObjBuilder b;
                b.appendMinKey( "" );
                return b.obj();
            }
            static BSONObj maxObject() {
                BSONObjBuilder b;
                b.appendMaxKey( "" );
                return b.obj();
            }
            bool advance() {
                int i = _i.size() - 1;
                while( i >= 0 && _i[ i ] >= ( (int)_v._ranges[ i ].intervals().size() - 1 ) ) {
                    --i;
                }
                if( i >= 0 ) {
                    _i[ i ]++;
                    for( unsigned j = i + 1; j < _i.size(); ++j ) {
                        _i[ j ] = 0;
                    }
                }
                else {
                    _i[ 0 ] = _v._ranges[ 0 ].intervals().size();
                }
                return ok();
            }
            /**
             * @return Suggested advance method, based on current key.
             *   -2 Iteration is complete, no need to advance.
             *   -1 Advance to the next key, without skipping.
             *  >=0 Skip parameter.  If @return is r, skip to the key comprised
             *      of the first r elements of curr followed by the (r+1)th and
             *      remaining elements of cmp() (with inclusivity specified by
             *      the (r+1)th and remaining elements of inc()).  If after() is
             *      true, skip past this key not to it.
             */
            int advance( const BSONObj &curr );
            const vector< const BSONElement * > &cmp() const { return _cmp; }
            const vector< bool > &inc() const { return _inc; }
            bool after() const { return _after; }
            void prepDive();
            void setZero( int i ) {
                for( int j = i; j < (int)_i.size(); ++j ) {
                    _i[ j ] = 0;
                }
            }
            void setMinus( int i ) {
                for( int j = i; j < (int)_i.size(); ++j ) {
                    _i[ j ] = -1;
                }
            }
            bool ok() {
                return _i[ 0 ] < (int)_v._ranges[ 0 ].intervals().size();
            }
            BSONObj startKey() {
                BSONObjBuilder b;
                for( int unsigned i = 0; i < _i.size(); ++i ) {
                    const FieldInterval &fi = _v._ranges[ i ].intervals()[ _i[ i ] ];
                    b.appendAs( fi._lower._bound, "" );
                }
                return b.obj();
            }
            // temp
            BSONObj endKey() {
                BSONObjBuilder b;
                for( int unsigned i = 0; i < _i.size(); ++i ) {
                    const FieldInterval &fi = _v._ranges[ i ].intervals()[ _i[ i ] ];
                    b.appendAs( fi._upper._bound, "" );
                }
                return b.obj();
            }
            // check
        private:
            const FieldRangeVector &_v;
            vector< int > _i;
            vector< const BSONElement* > _cmp;
            vector< bool > _inc;
            bool _after;
        };
    private:
        int matchingLowElement( const BSONElement &e, int i, bool direction, bool &lowEquality ) const;
        bool matchesElement( const BSONElement &e, int i, bool direction ) const;
        vector< FieldRange > _ranges;
        const IndexSpec &_indexSpec;
        int _direction;
        vector< BSONObj > _queries; // make sure mem owned
    };

    /**
     * As we iterate through $or clauses this class generates a FieldRangeSet
     * for the current $or clause, in some cases by excluding ranges that were
     * included in a previous clause.
     */
    class FieldRangeOrSet {
    public:
        FieldRangeOrSet( const char *ns, const BSONObj &query , bool optimize=true );

        /**
         * @return true iff we are done scanning $or clauses.  if there's a
         * useless or clause, we won't use or index ranges to help with scanning.
         */
        bool orFinished() const { return _orFound && _orSets.empty(); }
        /** Iterates to the next $or clause by removing the current $or clause. */
        void popOrClause( const BSONObj &indexSpec = BSONObj() );
        /** @return FieldRangeSet for the current $or clause. */
        FieldRangeSet *topFrs() const {
            FieldRangeSet *ret = new FieldRangeSet( _baseSet );
            if (_orSets.size()) {
                *ret &= _orSets.front();
            }
            return ret;
        }
        /**
         * @return original FieldRangeSet for the current $or clause. While the
         * original bounds are looser, they are composed of fewer ranges and it
         * is faster to do operations with them; when they can be used instead of
         * more precise bounds, they should.
         */
        FieldRangeSet *topFrsOriginal() const {
            FieldRangeSet *ret = new FieldRangeSet( _baseSet );
            if (_originalOrSets.size()) {
                *ret &= _originalOrSets.front();
            }
            return ret;
        }
        
        /** @ret a returned vector of simplified queries for all clauses. */
        void allClausesSimplified( vector< BSONObj > &ret ) const {
            for( list< FieldRangeSet >::const_iterator i = _orSets.begin(); i != _orSets.end(); ++i ) {
                if ( i->matchPossible() ) {
                    ret.push_back( i->simplifiedQuery() );
                }
            }
        }
        string getSpecial() const { return _baseSet.getSpecial(); }

        bool moreOrClauses() const { return !_orSets.empty(); }
    private:
        FieldRangeSet _baseSet;
        list< FieldRangeSet > _orSets;
        list< FieldRangeSet > _originalOrSets;
        list< FieldRangeSet > _oldOrSets; // make sure memory is owned
        bool _orFound;
    };

    /** returns a string that when used as a matcher, would match a super set of regex()
        returns "" for complex regular expressions
        used to optimize queries in some simple regex cases that start with '^'

        if purePrefix != NULL, sets it to whether the regex can be converted to a range query
    */
    string simpleRegex(const char* regex, const char* flags, bool* purePrefix=NULL);

    /** returns the upper bound of a query that matches prefix */
    string simpleRegexEnd( string prefix );

    long long applySkipLimit( long long num , const BSONObj& cmd );

} // namespace mongo
