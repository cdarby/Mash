#include "CommandFind.h"
#include "Index.h"
#include <zlib.h>
#include "kseq.h"
#include <iostream>
#include <set>
#include "pthread_pool.h"

using namespace::std;

KSEQ_INIT(gzFile, gzread)

CommandFind::CommandFind()
{
    name = "find";
    description = "Compare query sequences to a reference index";
    argumentString = "index.mash fast(a|q)[.gz] ...";
    
    addOption("help", Option(Option::Boolean, "h", "Help", ""));
    addOption("threshold", Option(Option::Number, "t", "Threshold. This fraction of the query sequence's min-hashes must appear in a query-sized window of a reference sequence for the match to be reported.", "0.2"));
    addOption("threads", Option(Option::Number, "p", "Parallelism. This many threads will be spawned to perform the find, each one handling on query sequence at a time.", "1"));
}

int CommandFind::run() const
{
    if ( arguments.size() < 2 || options.at("help").active )
    {
        print();
        return 0;
    }
    
    float threshold = options.at("threshold").getArgumentAsNumber(0, 1);
    int threads = options.at("threads").getArgumentAsNumber();
    
    Index index;
    //
    index.initFromCapnp(arguments[0].c_str());
    
    int l;
    int count = 0;
    
    void * pool = pool_start(find, threads);
    
    for ( int i = 1; i < arguments.size(); i++ )
    {
        gzFile fp = gzopen(arguments[i].c_str(), "r");
        kseq_t *seq = kseq_init(fp);
        
        while ((l = kseq_read(seq)) >= 0)
        {
            if ( l < index.getKmerSize() )
            {
                continue;
            }
            
            //printf("Query name: %s\tlength: %d\n\n", seq->name.s, l);
            //if (seq->comment.l) printf("comment: %s\n", seq->comment.s);
            //printf("seq: %s\n", seq->seq.s);
            //if (seq->qual.l) printf("qual: %s\n", seq->qual.s);
            
            pool_enqueue(pool, new FindData(index, seq->seq.s, l, threshold), true);
            //FindData * data = new FindData(index, seq->seq.s, l, threshold);
            //find(data);
            //delete data;
            //cout << endl;
        }
        
        if ( l != -1 )
        {
            printf("ERROR: return value: %d\n", l);
            return 1;
        }
        
        kseq_destroy(seq);
        gzclose(fp);
    }
    
    pool_wait(pool);
    pool_end(pool);
    
    return 0;
}

void * find(void * arg)
{
	CommandFind::FindData * data = (CommandFind::FindData *)arg;
	
    typedef unordered_map < uint32_t, set<uint32_t> > PositionsBySequence_umap;
    
    Index::Hash_set minHashes;
    
    const Index & index = data->index;
    int kmerSize = index.getKmerSize();
    float compressionFactor = index.getCompressionFactor();
    int length = data->length;
    char * seq = data->seq;
    float threshold = data->threshold;
    
    int mins = length / compressionFactor;
    //
    if ( mins < 1 )
    {
        mins = 1;
    }
    
    //cout << "Mins: " << mins << "\t length: " << length << "\tComp: " << compressionFactor << endl;
    getMinHashes(minHashes, seq, length, 0, kmerSize, compressionFactor);
    
    // get sorted lists of positions, per reference sequence, that have
    // mutual min-hashes with the query
    //
    PositionsBySequence_umap hits;
    //
    for ( Index::Hash_set::const_iterator i = minHashes.begin(); i != minHashes.end(); i++ )
    {
        Index::hash_t hash = *i;
        //cout << "Hash " << hash << endl;
        
        if ( index.getLociByHash().count(hash) != 0 )
        {
            for ( int j = 0; j < index.getLociByHash().at(hash).size(); j++ )
            {
                const Index::Locus & locus = index.getLociByHash().at(hash).at(j);
                
                //cout << "Match for hash " << hash << "\t" << locus.sequence << "\t" << locus.position << endl;
                hits[locus.sequence].insert(locus.position); // set will be created if needed
            }
        }
    }
    
    for ( PositionsBySequence_umap::iterator i = hits.begin(); i != hits.end(); i++ )
    {
        // pointer to the position at the beginning of the window; to be updated
        // as the end of the window is incremented
        //
        set<uint32_t>::const_iterator windowStart = i->second.begin();
        
        //cout << "Clustering in seq " << i->first << endl;
        
        // the number of positions between the window start and end (inclusive)
        //
        int windowCount = 0;
        
        for ( set<uint32_t>::const_iterator j = i->second.begin(); j != i->second.end(); j++ )
        {
            windowCount++;
            
            //cout << *windowStart << "\t" << *j << endl;
            
            // update window start if it is too far behind
            //
            while ( windowStart != j && *j > length && *windowStart < *j - length + 1 )
            {
                //cout << "moving " << *j - length + 1 << endl;
                windowStart++;
                windowCount--;
            }
            
            // extend the right of the window if possible
            //
            while ( j != i->second.end() && *j - *windowStart < length )
            {
                windowCount++;
                j++;
            }
            //
            windowCount--;
            j--;
            
            //cout << *windowStart << "\t" << *j << endl;
            if ( float(windowCount) / mins >= threshold )
            {
                cout << "   Cluster in ref " << i->first << "\tpos: " << *windowStart << "-" << *j << "\tscore: " << float(windowCount) / mins << endl;
                break;
                for ( set<uint32_t>::const_iterator k = windowStart; k != i->second.end() && *k <= *j; k++ )
                {
                    cout << "      " << *k << endl;
                }
            }
        }
    }
    
    return 0; // TODO: thread-safe results
}