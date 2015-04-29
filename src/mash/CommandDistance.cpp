#include "CommandDistance.h"
#include "Sketch.h"
#include <iostream>
#include <zlib.h>
#include "ThreadPool.h"

using namespace::std;

CommandDistance::CommandDistance()
: Command()
{
    name = "dist";
    description = "Compute the global distance of each query sequence to the reference. Both the reference and queries can be fasta or fastq, gzipped or not. The reference can also be a mash sketch file (.msh). If the reference is fasta/fastq and a sketch file does not exist (or is out of date), one will be written with the current options and used for future runs if possible. The score is computed as the number of matching min-hashes divided by -m, or the larger number of kmers if both sequences have fewer than -m.";
    argumentString = "<reference> <query> [<query>] ...";
    
    useOption("help");
    useOption("threads");
    useOption("kmer");
    useOption("mins");
    useOption("concat");
}

int CommandDistance::run() const
{
    if ( arguments.size() < 2 || options.at("help").active )
    {
        print();
        return 0;
    }
    
    int threads = options.at("threads").getArgumentAsNumber();
    int kmerSize = options.at("kmer").getArgumentAsNumber();
    int mins = options.at("mins").getArgumentAsNumber();
    bool concat = options.at("concat").active;
    
    Sketch sketch;
    
    const string & fileReference = arguments[0];
    
    if ( hasSuffix(fileReference, suffixSketch) )
    {
        if ( options.at("kmer").active || options.at("mins").active || options.at("concat").active )
        {
            cerr << "ERROR: The options " << options.at("kmer").identifier << ", " << options.at("mins").identifier << " and " << options.at("concat").identifier << " cannot be used when a sketch is provided; these are inherited from the sketch.\n";
            return 1;
        }
        
        sketch.initFromCapnp(fileReference.c_str());
    }
    else
    {
        bool sketchFileExists = sketch.initHeaderFromBaseIfValid(fileReference, false);
    
        if
        (
            (options.at("kmer").active && kmerSize != sketch.getKmerSize()) ||
            (options.at("mins").active && mins != sketch.getMinHashesPerWindow())
        )
        {
            sketchFileExists = false;
        }
    
        if ( false && sketchFileExists )
        {
            sketch.initFromBase(fileReference, false);
            kmerSize = sketch.getKmerSize();
            mins = sketch.getMinHashesPerWindow();
        }
        else
        {
            vector<string> refArgVector;
            refArgVector.push_back(fileReference);
        
            //cerr << "Sketch for " << fileReference << " not found or out of date; creating..." << endl;
            cerr << "Sketching " << fileReference << " (provide sketch file made with \"mash sketch\" to skip)...\n";
            
            sketch.initFromSequence(refArgVector, kmerSize, mins, false, 0, concat);
            /*
            if ( sketch.writeToFile() )
            {
                cerr << "Sketch saved for subsequent runs." << endl;
            }
            else
            {
                cerr << "The sketch for " << fileReference << " could not be saved; it will be sketched again next time." << endl;
            }*/
        }
    }
    
    ThreadPool<CompareInput, CompareOutput> threadPool(compare, threads);
    
    for ( int i = 1; i < arguments.size(); i++ )
    {
        if ( hasSuffix(arguments[i], suffixSketch) )
        {
            Sketch sketchQuery;
            sketchQuery.initFromCapnp(arguments[i].c_str(), true);
            
            if ( sketchQuery.getKmerSize() != sketch.getKmerSize() )
            {
                cerr << "\nWARNING: The query sketch " << arguments[i] << " has a kmer size (" << sketchQuery.getKmerSize() << ") that does not match the reference sketch (" << sketch.getKmerSize() << "). This query will be skipped.\n\n";
                continue;
            }
            
            if ( sketchQuery.getMinHashesPerWindow() != sketch.getMinHashesPerWindow() )
            {
                cerr << "\nWARNING: The query sketch " << arguments[i] << " has a min-hash count (" << sketchQuery.getMinHashesPerWindow() << ") that does not match the reference sketch (" << sketch.getMinHashesPerWindow() << "). This query will be skipped.\n\n";
                continue;
            }
            
            if ( sketchQuery.getConcatenated() != sketch.getConcatenated() )
            {
                cerr << "\nWARNING: The query sketch " << arguments[i] << " is concatenated, but the reference is not. This query will be skipped.\n\n";
                continue;
            }
        }
        
        for ( int j = 0; j < sketch.getReferenceCount(); j++ )
        {
            threadPool.runWhenThreadAvailable(new CompareInput(sketch.getReference(j).hashes, sketch.getReference(j).name, arguments[i], sketch.getKmerSize(), sketch.getMinHashesPerWindow(), sketch.getConcatenated()));
        
            while ( threadPool.outputAvailable() )
            {
                writeOutput(threadPool.popOutputWhenAvailable());
            }
        }
    }
    
    while ( threadPool.running() )
    {
        writeOutput(threadPool.popOutputWhenAvailable());
    }
    
    return 0;
}

void CommandDistance::writeOutput(CompareOutput * output) const
{
    for ( int i = 0; i < output->pairs.size(); i++ )
    {
        cout << output->pairs.at(i).score << '\t' << output->nameRef << '\t' << output->pairs.at(i).file << endl;
    }
    
    delete output;
}

CommandDistance::CompareOutput * compare(CommandDistance::CompareInput * data)
{
    const Sketch::Hash_set & minHashesRef = data->minHashesRef;
    const string file = data->file;
    
    CommandDistance::CompareOutput * output = new CommandDistance::CompareOutput();
    Sketch sketch;
    
    if ( hasSuffix(file, suffixSketch) )
    {
        sketch.initFromCapnp(file.c_str());
    }
    else
    {
        vector<string> fileVector;
        fileVector.push_back(file);
    
        sketch.initFromSequence(fileVector, data->kmerSize, data->mins, false, 0, data->concat);
    }
    
    output->nameRef = data->nameRef;
    output->pairs.resize(sketch.getReferenceCount());
    
    for ( int i = 0; i < sketch.getReferenceCount(); i++ )
    {
        int common = 0;
        
        const Sketch::Hash_set & minHashes = sketch.getReference(i).hashes;
        
        for ( Sketch::Hash_set::const_iterator i = minHashes.begin(); i != minHashes.end(); i++ )
        {
            if ( minHashesRef.count(*i) == 1 )
            {
                common++;
            }
        }
        
        int denominator;
        
        if ( minHashesRef.size() >= data->mins && minHashes.size() >= data->mins )
        {
            denominator = data->mins;
        }
        else
        {
            if ( minHashesRef.size() > minHashes.size() )
            {
                denominator = minHashesRef.size();
            }
            else
            {
                denominator = minHashes.size();
            }
        }
        
        output->pairs[i].score = float(common) / denominator;
        output->pairs[i].file = sketch.getReference(i).name;
    }
    
    return output;
}
